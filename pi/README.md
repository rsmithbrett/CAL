# Raspberry Pi Zero client workstream

This directory is reserved for a native Linux display client. There is now a **development fixture** executable (`build/pi-card`). Without live arguments it renders one labeled sample card with a software SDL2/SDL_ttf backend. Live mode below checks in and rotates server-assigned forecasts; assets and OTA remain outstanding. On Raspberry Pi OS 32-bit, install the SDL2 and SDL_ttf development packages plus DejaVu Sans, then run `make -C pi demo` and `pi/build/pi-card` on an attached display. For a headless fixture: `SDL_VIDEODRIVER=dummy pi/build/pi-card --width 1280 --height 720 --once /tmp/pi.bmp`. `src/ScheduleCore.h` is the first shared scheduling slice and is compiled by the ESP32 `App/CardManager.cpp`; `tests/schedule_core_test.cpp` checks the same header on a host compiler. Run `make -C pi test` for that check. The branch CI also runs `bash ci/build-firmware.sh` to guard the firmware integration. Start with the repository-root `PI_ZERO_CLIENT_DESIGN.md` and `PI_ZERO_CLIENT_TEST_PLAN.md`, then the server repository's onboarding, API console and formal client specification.

The target is Raspberry Pi OS 32-bit compatibility with the original Zero/Zero W; Zero 2 W is a separate measured profile. SDL is the current provisional backend; the package channel and physical acceptance remain outstanding. Keep the same scheduler source shared with ESP32 `App/`, with Pi-specific rendering, input, networking and storage adapters.

The **base product is the original Raspberry Pi Zero**, with its single-core ARM1176 CPU and 32-bit OS. `make -C pi test-armv6` cross-builds the shared scheduler and layout tests for ARMv6 soft-float and runs them with QEMU's ARM1176 user-mode CPU. On Ubuntu install `g++-arm-linux-gnueabi qemu-user` first. The `armv6-emulation` CI job runs this CPU-compatibility gate on every branch push. Ubuntu's `gnueabihf` runtime contains newer ARM instructions and fails on the emulated ARM1176, so this gate does **not** prove compatibility with Raspberry Pi OS's hard-float ABI. Cross-building the real display executable against a pinned Raspberry Pi OS ARMv6 sysroot is the next gate. The plain Zero has no built-in Wi-Fi, so live server access later needs a chosen USB network adapter or another explicit network path. The Zero W and Zero 2 W are optional follow-on profiles, not the baseline. This process-level emulation does not boot Raspberry Pi OS or validate SDL rendering, HDMI, USB networking, or performance on a physical Zero.

The shelved Android experiment is preserved in closed PR https://github.com/rsmithbrett/CAL/pull/2. This branch starts from `main` and does not depend on that PR or merge its Android scaffold.

The `armv6-display` CI job also runs the real SDL fixture inside an emulated ARMv6 hard-float Alpine container and publishes its 320×240 and 1280×720 BMP frames as a workflow artifact. Alpine's musl runtime is **not** the Raspberry Pi OS glibc runtime: this checks that the display source executes on an ARMv6 hard-float CPU, while a Pi OS sysroot build and physical HDMI test remain separate gates.

The separate `pi-zero-raspios.yml` workflow downloads the pinned official 32-bit Raspberry Pi OS Lite image, verifies its published SHA-256, then builds and renders the fixture inside that image under QEMU. It runs when the credential reader, tests, Makefile, or its own script/workflow changes, and by manual dispatch; the image and package installation are much larger than the fast per-commit gates. A passing run validates the Raspberry Pi OS user-space ABI and SDL libraries, but does not boot the Pi kernel or establish physical HDMI performance.

`InstallationConfig.h` is the first credential storage boundary for a future provisioned Pi. It accepts an owner-only regular file (mode `0600` or stricter), refuses symlinks, and parses exactly `installationId=<UUID>` and `deviceSecret=<43-character unpadded base64url>` on separate newline-terminated lines. The secret comes from the server's one-time provisioning response; this fixture does not enroll or contact a server. Store one unique file per installation, never commit one or reuse an ESP32 credential. `make -C pi test` and `test-armv6` exercise distinct installations and refusal behavior; the Raspberry Pi OS workflow also runs the tests inside the pinned image. No production secret is needed for these tests.

`make -C pi checkin` builds the first check-in **transport probe** (requires libcurl development headers). Once the server PR is deployed and an installation is provisioned and activated, run `pi/build/pi-checkin https://SERVER PRIVATE_CREDENTIAL_FILE PACKAGE_VERSION`. It loads the private credential and posts an HTTPS JSON heartbeat to `/api/checkin` with `X-Device-Secret`; certificate verification, timeouts and a response-size cap are enforced. It reports the HTTP status and byte count without printing the secret or returned content. This probe validates acknowledgment and policy but does **not** execute actions, and a local build is not evidence of a live check-in. Use a unique credential file for each Pi. Do not point the probe at a production server during development.

`make -C pi test-checkin-transport` runs the actual native probe twice against a
loopback fake with distinct temporary credentials, revokes one fake identity,
and verifies the other still checks in. Only the separately compiled test
binary can use plaintext on numeric `127.0.0.1`; the normal binary still
requires HTTPS. This is a transport regression, **not** the Bolt 1 exit test:
the fake does not exercise the server's provisioning, policy resolution, or
revocation implementation.

For a live check, the native probe can write successful response bytes to a new
owner-only file using `--response-file PATH`; it refuses existing paths and
symlinks. The verifier `pi/tests/checkin_live_test.py` parses those exact bytes,
requires acknowledgment, and checks each installation's expected policy without
printing returned content. Give it the native binary, HTTPS origin, two private
credential files, and two JSON files describing distinct expected `cardPolicy`
objects (or subsets). Run again with `--revoked-first` after deactivating only the
first dedicated test device. The second policy must still match. A host run is
native integration evidence, not ARMv6 emulation or physical Zero acceptance.

## Native policy boundary

Install `nlohmann-json3-dev` alongside the libcurl development package. `make test` and `make test-armv6` include the bounded policy decoder and replacement-state tests. `pi-checkin` now requires an acknowledged, valid policy response before saving private evidence, and reports only validity/presence/count. Unknown card IDs and optional policy fields are retained for the upcoming registry adapter; they are not rendered by this slice. Missing/null policy keeps current state, while an explicit empty cards array replaces it with empty state.

## Live forecast display

`make -C pi demo` now also links libcurl and nlohmann/json. With a dedicated activated development Pi identity, run:

```sh
pi/build/pi-card --server https://DEV_SERVER --credentials /private/pi.credentials --version 0.1.0
```

`--once /private/frame.bmp` performs one check-in/provider pass and saves its frame for acceptance. Screenshots contain location/weather; keep them private. Omitting all three live arguments retains the explicitly labeled fixture mode. Live mode never substitutes sample values. Forecast/forecast2–5 follow the server's order, dwell, interleave and home/target policy. Unsupported card IDs are counted and skipped. HTTP authorization denial clears state; temporary failures mark retained content as last received. Refresh is once per 60 seconds; the existing weather endpoint is reused, with no API keys on the client. Press Escape to exit. No physical Pi, asset, watch/actions, offline disk cache or OTA acceptance is claimed.

Install `libcurl4-openssl-dev nlohmann-json3-dev` with SDL dependencies. `make -C pi test test-armv6 test-live-display` verifies the new state machine and isolated SDL wire flow. The server PR must be deployed to a development instance for actual-server acceptance; do not use production credentials in fixtures.
