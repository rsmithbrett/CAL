# Raspberry Pi Zero display client — design proposal

Status: first design gate only. No Pi executable, server enrollment, live cards, hardware run, or package update is implemented on this branch.

## Goal and scope

Build a Linux display client for Discover Around Me that shows the server-assigned cards and graphics on an attached screen. Every installation is a separate device with its own revocable identity, owner/brand/account assignment, policy, action queue, event cursor, diagnostics, and update state. An Android installation or ESP32 panel cannot supply its credential to a Pi.

The current development baseline is Raspberry Pi OS **32-bit** on a Pi Zero/Zero W, so the architecture does not silently require a 64-bit Zero 2 W. Zero 2 W is a second test profile with the same protocol and renderer. The exact first physical board, display and input device remain to be identified before a hardware acceptance claim. Original Zero/Zero W is a single-core Armv6 device with 512 MB RAM; Zero 2 W has a quad-core Cortex-A53 with 512 MB. Measure boot, memory, frame rate, decoding, and network behavior on both before promising equal performance. Raspberry Pi OS 32-bit supports the older Zero class: https://www.raspberrypi.com/documentation/computers/os.html ; hardware: https://www.raspberrypi.com/products/raspberry-pi-zero-2-w/ and https://www.raspberrypi.com/products/raspberry-pi-zero-w/ .

This is a separate Linux package and service, not CAL's ESP32 loader and not the shelved Android APK. CAL's `App/` is the current behavioral reference. Keep this branch separate from CAL `main`, which creates firmware releases on push. The server's formal Device Client Specification and live API console govern the wire. Update those when an actual Pi route/field is introduced, not merely from this proposal.

## Client boundaries

1. **Identity and activation:** a Pi installation has a server record, unique secret, platform/package version, activation, assignment, and revocation lifecycle. Do not synthesize a MAC address to pass physical-only enrollment or reuse an ESP32 secret. The existing server check-in route currently requires a physical-device identity, so live calls are gated on a server change. A normal package update retains identity; a fresh install or erased storage receives a new identity unless a separately authorized transfer flow exists.
2. **Transport and state:** one adapter owns TLS, authentication, timeouts, backoff, local persisted state and bounded logs. The client uses authenticated check-in to obtain resolved policy/actions and `GET /api/device/watch` JSON long polling with a cursor while running; it reconciles from a snapshot and refreshes after reconnect. Inbound webhooks are not a client push channel. A durable pending-action queue retains each stable instance ID until `acceptedActionIds` acknowledges it. Never log calendar details or expose secrets in diagnostics.
3. **Card registry and scheduler:** the server supplies card order, dwell, interleave, content, actions, branding and assets. Extract the actual scheduler source from `App/CardManager.cpp` behind platform adapters and compile that same source into ESP32 and Pi. Do not copy a second scheduler or mistake a static card picture for parity. Unknown card IDs are skipped with a bounded compatibility report.
4. **Renderer and input:** measure the actual framebuffer/display geometry, density, safe area and orientation at runtime. Select and verify server asset variants/hashes; preserve aspect ratio and semantic content. Do not upscale the ESP32 320 × 240 frame or crop QR codes, logos, captions, disclaimers or actions. Evaluate a small native Linux renderer on real hardware before fixing the graphics backend; avoid assuming a Chromium kiosk can meet the original Zero memory and latency budget. Normalize touch, keyboard, buttons and HDMI-CEC remote input into shared actions only where that input actually exists. Provide visible focus and a clear no-input fallback.
5. **Configuration and locality:** server policy and brand/account inheritance remain authoritative. The client caches only the last permitted display state for offline use, marks stale/unavailable data clearly, and bounds asset cache size, writes, telemetry and log retention. Device-specific screen behavior is an adapter/capability, not a forked card policy. Locale content follows the operative server contract; do not invent a translation-file dependency.
6. **OTA and recovery:** app updates are signed Linux packages distributed through a controlled, authenticated repository or managed fleet channel, installed by an explicitly configured updater and restarted under systemd. Package and OS security updates have separate policies; the server must not offer an ESP32 firmware image to a Pi. Define channel, package signing, monotonic versions, staged rollout, update window, health check, rollback to a retained known-good package, interrupted install and SD-card failure behavior before unattended rollout. No remote arbitrary shell commands, plain APK/ESP32 binaries, or unsigned scripts. Server card/graphic changes normally require no client package update.

## Delivery stages

1. Confirm first target board (Zero/Zero W versus Zero 2 W), Raspberry Pi OS image, HDMI/display resolution, Wi-Fi, input/remote and power. Capture baseline cold boot, memory and image-decode measurements. Use an isolated development identity and database.
2. Extract scheduler behavior to shared C++ and build host tests plus ESP32 regression. Record order, interleave, history, dwell, manual navigation, policy replacement and empty-card semantics.
3. Add Pi platform adapters and a headless frame fixture, then validate the actual HDMI display and input on a physical Zero. A host build or screenshot alone does not pass the hardware gate.
4. Specify and implement server-side Pi identity/enrollment/check-in with authorization and tenant-isolation contract tests. Update the formal client specification and developer API docs in the same change. Keep ESP32 compatibility explicit.
5. Show the first live card with its provider data and server graphic, then add the registered card set, watch/reconcile, durable actions, stale/offline behavior, localization and branding.
6. Package the client and systemd service; test signed over-the-air update, health validation, rollback and recovery on hardware before any wider deployment.

## Process

Follow `ONBOARDING.md`, `README.md`, `CI.md` and `TEST_PLAN.md` in the server repository, and CAL's firmware build procedure. Record design and tests before behavior changes; keep feature code on isolated branches. Run both the new host/Pi checks and `bash ci/build-firmware.sh` for shared-source changes. Read printed test/build summaries, not only exit codes. A fresh CI build is not a physical Pi demonstration. No production credential or production database belongs in development fixtures.

## Emulation boundary

Use QEMU `raspi0` for original Zero-like ARM1176/512 MB system checks and QEMU ARM user-mode for cross-built process tests. QEMU currently lists no Zero 2 W machine; `raspi3ap` is an approximate Cortex-A53/512 MB profile, not certification of a Zero 2 W. The emulated framebuffer can exercise layout fixtures; emulated CPU timing, HDMI-CEC, Wi-Fi radio, GPIO wiring, power, thermal behavior and SD recovery do not establish physical-device behavior. Keep QEMU fixtures in CI as automated checks and reserve the final display/input/update gate for a real board. Reference: https://www.qemu.org/docs/master/system/arm/raspi.html and https://www.qemu.org/docs/master/user/main.html .

## First coding slice: shared due-card logic

Extract the existing `CardManager` active-counter tick and due-interstitial selection into `pi/src/ScheduleCore.h` and call that exact header from the ESP32 `App/CardManager.cpp`. This is a narrow shared-source seam, not the complete scheduler or a Pi executable. Preserve the existing showable filter, order tie break, threshold (`cardsSince > interleaveEvery`), saturation, and reset-on-show behavior. Next extract list cursor and history/dwell transitions without forking behavior.
