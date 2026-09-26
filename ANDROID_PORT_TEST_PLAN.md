# Android display client test plan

Status: first build scaffold. The checks below distinguish what is implemented from what is planned. A green scaffold build does not prove live API or card parity.

## Scaffold checks

| Check | Method | Pass condition |
|---|---|---|
| Android debug build | `gradle -p android assembleDebug` on a fresh CI runner with JDK 17 and Android SDK/NDK installed | APK produced; build log has no errors |
| Native bridge | Emulator instrumented test or manual launch | Activity obtains native card title through JNI and displays it |
| Runtime layout | Screenshots at phone, tablet, 720p and 1080p TV emulator sizes | Sample remains legible with safe margins; final renderer adapts content without stretching images or clipping captions/actions |
| TV launch/input | TV emulator launches from its launcher; D-pad arrows, Select and Back are exercised | Every action is reachable with visible focus, no touchscreen dependency |
| Existing firmware | `bash ci/build-firmware.sh` | CAL and App compile; actual partition ceilings and checksums reported |

Only the Android build job is introduced with this scaffold. Emulator launch, native call, and firmware regression remain unverified until their jobs or bench runs are recorded on this branch. Do not report them as passed on the strength of an APK build.

## Shared scheduler proof (next slice)

An automated host test must compile the same scheduler source that the ESP32 App uses. Inject time and card callbacks. Assert list order, independent interstitial counters and order tie-breaks, notable dwell, forward/rewind history replay, item-count changes resetting stale history, manual hold, unknown card IDs, `null` policy retaining the last policy, and a zero-item card never rendering blank. Capture the test count and run the ESP32 build separately.

## Cloud integration (after identity design)

Test registration, pending activation, revocation, secret rejection, discovery, exact camelCase contract, `cardPolicy` omitted/null versus empty, persisted press IDs across process death, deduplicated retry until `acceptedActionIds`, event cursor timeout, reconcile snapshot, reconnect after network loss, and foreground resume. Enroll two Android installations for the same owner and prove distinct records, secrets, assignments, card policies, queues, cursors, and diagnostics; revoke or reassign one and prove the other is unchanged. A reinstall must create a new device unless a secure recovery flow is explicitly implemented. Use an isolated Development Host and database. Never use an ESP32 device secret as an Android fixture.

## Card and UX acceptance

For each card ID, compare real App content with Android at a known fixture input and capture phone/tablet/TV screenshots. Feed the same server graphic and metadata to 4:3, 16:9, low-resolution, and high-resolution viewports: assert aspect-preserving contain by default, explicit crop only when allowed, correct variant selection and hash verification, bounded cache/decode behavior, and preservation of QR codes, captions, brand marks, disclaimers, banners, and actions. Include day/night colors, empty/refused/stale states, long text, localization, and required qualifiers such as the Home Value estimate disclaimer. Calendar titles must stay off telemetry and action summaries. Check touch and remote focus/navigation, display safe areas, accessibility, and offline behavior. Finish with one physical Android-based TV device for remote control, sustained running, resume, and power behavior. Do not report Vega OS compatibility from an Android emulator.

## Reporting

Record the exact commit SHA, Android plugin/Gradle/NDK versions, SDK level, emulator API/profile, test counts, artifact checksum, and any check not run. Read the printed build and test summaries rather than relying solely on process exit status. Do not describe an emulator pass as physical-device verification.
