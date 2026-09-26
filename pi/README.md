# Raspberry Pi Zero client workstream

This directory is reserved for a native Linux display client. There is no Pi display executable here yet. `src/ScheduleCore.h` is the first shared scheduling slice and is compiled by the ESP32 `App/CardManager.cpp`; `tests/schedule_core_test.cpp` checks the same header on a host compiler. Run `make -C pi test` for that check. The branch CI also runs `bash ci/build-firmware.sh` to guard the firmware integration. Start with the repository-root `PI_ZERO_CLIENT_DESIGN.md` and `PI_ZERO_CLIENT_TEST_PLAN.md`, then the server repository's onboarding, API console and formal client specification.

The first build target is Raspberry Pi OS 32-bit compatibility with the original Zero/Zero W; Zero 2 W is a separate measured profile. No graphics backend, package channel or enrollment contract is selected by the placeholder directory. Keep the same scheduler source shared with ESP32 `App/`, with Pi-specific rendering, input, networking and storage adapters.

The shelved Android experiment is preserved in closed PR https://github.com/rsmithbrett/CAL/pull/2. This branch starts from `main` and does not depend on that PR or merge its Android scaffold.
