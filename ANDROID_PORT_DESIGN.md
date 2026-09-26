# Android display client: first portability slice

Status: design and build scaffold. No cloud enrollment, card data, or production release is implemented by this slice.

## Purpose

Prove that an Android application can display Discover Around Me cards while reusing behavior from `App/` where the code can be separated from ESP32 hardware. The source of truth for device behavior remains the Device Client Specification in the server repository's `docs/` directory. The server's README describes the currently built HTTP contract; CAL's README describes the App's current behavior. Resolve any disagreement in favor of the formal client specification before implementing a protocol change.

## Boundaries and decisions

1. `CAL.ino` is the ESP32 loader. It is not an Android bootloader and is not part of the Android package. `App/` is the behavioral reference.
2. The Android application is a distinct client installation with its own identity and secret. It must not impersonate an existing panel or reuse its `X-Device-Secret`.
3. The server decides the resolved card policy, actions, content, and branding. Android renders those values; it does not hardcode a second server-side policy.
4. A button press enters a durable local pending queue and rides the existing `POST /api/checkin` `pendingActions` field. The queue removes an item only after its `instanceId` appears in `acceptedActionIds`. Calendar details must not enter the press summary, logs, or telemetry.
5. The Android app reads `GET /api/device/watch` with a cursor while foregrounded, handles `reconcile: true` as a snapshot, and refreshes on resume. A phone behind NAT does not receive inbound webhooks. Webhooks remain a separate server-to-server integration path.
6. Firmware OTA instructions are not Android application updates. Android release and installation use Android packaging and a separate version scheme.
7. No production endpoint, credential, or user data is embedded in the APK or CI configuration.

## Composable client architecture

The Android app is one client of the Discover Around Me platform, not an emulator of a particular ESP32 board. The server's [Android platform API design](https://github.com/rsmithbrett/DiscoverAroundMe/blob/docs/android-platform-api-contract/ANDROID_PLATFORM_API_DESIGN.md) records the current routes and proposed identity work; the live developer API console is maintained in `ApiEndpointDocs.cs`. No Android installation can use check-in yet: the current server route requires a physical device identity.

- **Client identity and transport:** one adapter owns installation credentials, authenticated API calls, version and capability reporting, retries, and offline cache policy. Each installation has its own revocable identity; no card stores a secret or calls a provider directly.
- **Card registry:** each server card ID resolves to a renderer, a data dependency, and optional interactions. The server supplies ordering, dwell, assets, localized content, and action definitions. Android selects a renderer and lays it out at the logical 320 x 240 card size. Unknown or disabled IDs are skipped with an aggregate compatibility report.
- **Shared behavior:** extract the scheduler and its policy transition rules into C++ that both ESP32 and Android compile. Display drawing, touch input, clock, storage, and networking stay behind platform adapters. Do not copy a fork of the scheduler into Android.
- **Events and actions:** one event adapter long polls the JSON `/api/device/watch` route and reconciles from a snapshot; one durable queue sends button presses with check-in until the server accepts their IDs. Webhook subscriptions are outgoing server integrations for external receivers, not mobile push delivery.
- **Capability and extension contract:** touch, motion, storage, battery, and display support are reported independently. A card's unsupported capability has a defined fallback. New cards require matching server catalog/policy, Android registration, ESP32 registration when applicable, wire documentation, tests, and a content privacy check. Android package updates do not follow the ESP32 firmware OTA path.

This separation lets a new provider, card, action destination, or client platform be introduced without adding a new tenant policy engine or changing unrelated renderers.

## Shared-code experiment

`App/Cards.h` describes card registration, policy, announcements, and draw/fetch callbacks. `App/CardManager.cpp` currently includes `Actions`, `Display`, `HeapRatchet`, `Log`, `Motion`, and `Touch`, and depends on Arduino `String` and timing. Compiling it for Android requires a deliberate interface boundary. A header-only or renamed copy of the scheduler does not prove behavior sharing.

The first behavioral extraction will separate scheduler state and transitions from the ESP32 adapters. The same C++ source file must be compiled into both the Arduino App and an Android NDK target. Platform adapters supply time, persisted pending actions, HTTP, rendering, touch, and logging. Keep ESP32 memory limits and the current 320 x 240 landscape layout in view. Do not alter rotation, dwell, interleave, history, or press behavior merely to make the extraction compile.

The proof is complete only when one registered card uses the shared scheduler on Android, changes position under an injected clock, and still builds for ESP32. A static demonstration screen is a build scaffold, not that proof.

## Android application shape

`android/` contains an Android application with a minimal Java Activity and an NDK/CMake library. The first scaffold renders a clearly labeled sample card at a 320 x 240 logical size and invokes native C++ through JNI. The sample screen has no cloud credentials and makes no network request. Subsequent slices replace sample state with the resolved server policy and data, one card at a time.

The app must scale the logical card without cutting off the card body or button row on phone and tablet viewports. A user may navigate with touch. It must remain clear when data is stale, unavailable, or not configured. Existing App wording that prevents operator diagnostic text or private calendar content reaching a household screen applies here as well.

## Integration gates

Before a live API slice, settle how an Android installation is created, activated, revoked, and assigned in the tenant hierarchy. Review the server's existing device kinds, enrollment route, `CheckInRequest`, `CheckInResponse`, discovery document, and `DeviceEvents` watch. Any server change is additive unless the current `DeploymentStage` explicitly permits a wire change, and the ESP32 App and server changes are reviewed together. Use an isolated Development Host, a separate Postgres database, and a dedicated test identity; never seed or mutate the production database for this port.

## Delivery sequence

1. Build this Android scaffold and launch it in an emulator. Record the APK and screenshot as evidence.
2. Extract the scheduler seam and test its list/interstitial order, history, dwell, policy replacement, and empty-card behavior on a host target. Rebuild CAL and App with `ci/build-firmware.sh`.
3. Implement dedicated Android enrollment and authenticated check-in on the server and Android side, with contract tests in both repositories.
4. Add Forecast as the first live card, then Listings, Home Value, Calendar, and the remaining registered card IDs. Validate screen privacy and content budgets with and without actions.
5. Add durable press queue, event-watch reconnection, assets, banners, branding, and offline handling. Test on emulator profiles and one real device before release.

## Repository process

Use an isolated feature branch. Update design and test plan before implementing each behavior; run the new test and the existing firmware build before a merge. CAL's `main` push automatically creates firmware releases, so an Android branch must not be merged merely because its APK builds. Verify the exact CI commit, including both Android and ESP32 jobs. Coordinate any server contract change in the server repository without adding a build dependency between repositories. The main server worktree may host production; use a separate worktree and scratch database for server changes. Follow `ONBOARDING.md`, `CI.md`, and the server and CAL `TEST_PLAN.md` files for release gates.
