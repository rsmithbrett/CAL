# Android portability build scaffold

This is a development-only APK. It displays a labeled sample card, calls a native C++ function, and compiles `App/Cards.h` through a small Android-only Arduino type shim. It does not yet implement the scheduler, cloud enrollment, live content, actions, or webhooks. See `../ANDROID_PORT_DESIGN.md` and `../ANDROID_PORT_TEST_PLAN.md` for the staged work and acceptance gates.

The build uses JDK 17, Gradle 8.13, Android Gradle plugin 8.13.2, SDK 36, NDK 27.0.12077973, and CMake 3.22.1. In Android Studio, open this `android/` directory and install those SDK components. From a command line with Gradle and the Android SDK installed:

```sh
gradle -p android :app:assembleDebug
```

The resulting APK is `android/app/build/outputs/apk/debug/app-debug.apk`. The GitHub Actions `android-port.yml` workflow builds the same APK from a fresh Linux runner and uploads it as a development artifact. The repository's separate `build-and-release.yml` remains the ESP32 firmware release pipeline; do not use its `latest` asset as an Android build.

For the emulator, create a phone or tablet virtual device with API 26 or later, install the debug APK, and check that the entire 320 x 240 logical card is visible. The current build has no network permission and cannot contact Discover Cloud. A successful launch only proves the Android shell and native descriptor header compile and load; the card manager's behavior has not yet been ported.
