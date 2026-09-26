# Android display client test plan

Status: first build scaffold. The checks below distinguish what is implemented from what is planned. A green scaffold build does not prove live API or card parity.

## Scaffold checks

| Check | Method | Pass condition |
|---|---|---|
| Android debug build | `gradle -p android assembleDebug` on a fresh CI runner with JDK 17 and Android SDK/NDK installed | APK produced; build log has no errors |
| Native bridge | Emulator instrumented test or manual launch | Activity obtains native card title through JNI and displays it |
| Logical card size | Emulator screenshot at a phone and tablet size | 320 x 240 logical card fits without clipping; sample label remains visible |
| Existing firmware | `bash ci/build-firmware.sh` | CAL and App compile; actual partition ceilings and checksums reported |

Only the Android build job is introduced with this scaffold. Emulator launch, native call, and firmware regression remain unverified until their jobs or bench runs are recorded on this branch. Do not report them as passed on the strength of an APK build.

## Shared scheduler proof (next slice)

An automated host test must compile the same scheduler source that the ESP32 App uses. Inject time and card callbacks. Assert list order, independent interstitial counters and order tie-breaks, notable dwell, forward/rewind history replay, item-count changes resetting stale history, manual hold, unknown card IDs, `null` policy retaining the last policy, and a zero-item card never rendering blank. Capture the test count and run the ESP32 build separately.

## Cloud integration (after identity design)

Test registration, pending activation, revocation, secret rejection, discovery, exact camelCase contract, `cardPolicy` omitted/null versus empty, persisted press IDs across process death, deduplicated retry until `acceptedActionIds`, event cursor timeout, reconcile snapshot, reconnect after network loss, and foreground resume. Use an isolated Development Host and database. Never use an ESP32 device secret as an Android fixture.

## Card and UX acceptance

For each card ID, compare real App content with Android at a known fixture input and capture phone/tablet screenshots. Include buttons, banners, day/night colors, graphics, empty/refused/stale states, long text, localization, and required qualifiers such as the Home Value estimate disclaimer. Calendar titles must stay off telemetry and action summaries. Check touch zones, display scaling, accessibility, and offline behavior on emulator. Finish with one physical Android device for touch, sustained running, and power behavior.

## Reporting

Record the exact commit SHA, Android plugin/Gradle/NDK versions, SDK level, emulator API/profile, test counts, artifact checksum, and any check not run. Read the printed build and test summaries rather than relying solely on process exit status. Do not describe an emulator pass as physical-device verification.
