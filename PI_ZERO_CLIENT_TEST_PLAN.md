# Raspberry Pi Zero client test plan

Status: proposed gates. No Pi executable, package, physical run, or new server contract exists yet.

## Target inventory and performance

- Record the exact original Zero/Zero W or Zero 2 W board revision, Raspberry Pi OS 32-bit image/version, attached display, resolution, input device, Wi-Fi, supply and storage. Run both hardware profiles before claiming cross-model support.
- Measure cold boot to first useful branded screen, idle/peak RSS memory, CPU, decode latency, card transition time, temperature, sustained run and SD writes. Set release budgets from measured baseline, then fail regression when exceeded.
- With network absent at boot and during operation, retain a usable cached screen with clear stale state, retry without reboot loop, and recover after network returns.

## Shared behavior and rendering

- Compile the **same** extracted scheduler source on host, ESP32 and Pi. Test list order, interstitial timing/tie breaks, notable dwell, forward/reverse history, manual hold, policy replacement, missing/unknown IDs and zero-item fallback. Run `bash ci/build-firmware.sh` and inspect both binary summaries.
- Feed fixture card/policy/data/asset content to 4:3 and 16:9, low and high resolution screens. Verify responsive text and image aspect ratio, variant/hash validation, bounded decode/cache and no clipped QR, logo, disclaimer, action or banner. Compare against ESP32 semantics rather than pixel equality.
- Exercise actual attached display, touch/keyboard/HDMI-CEC remote where present, visible focus and the no-input fallback. Capture physical evidence; host/headless frames do not count as a Pi screen pass.

## Server and identity

- In an isolated Development Host/database, enroll two Pis under one owner. Prove distinct device records/secrets, assignments, policies, action queues, cursors and diagnostics; revoke or reassign one without affecting the other. A reinstall creates a new identity unless a separately specified transfer is tested.
- Verify auth refusal for missing/revoked/wrong secret, tenant boundaries, Pi package version/capability fields, and that ESP32 physical enrollment/check-in and OTA still work. Pin omitted/null/empty policy compatibility in contract tests.
- Verify check-in, provider and asset reads, watch cursor 0/snapshot/reconcile/timeout/reconnect, action ID persistence and deduplication after a lost response. Keep private calendar content out of logs and action summaries.

## Updates and operations

- Verify only a valid signed package with a suitable version/channel installs. Stage to one test Pi, check service restart and healthy display, then widen. Interrupt download/install and simulate failed startup; restore a retained known-good version without losing installation identity, pending actions or cached display state.
- Confirm package updates never consume ESP32 firmware manifest/binary, server policy/graphics can change without a package release, logs/cache have retention limits, and OS updates follow their own maintenance policy.

## Reporting gate

For every run record exact CAL/server commit, OS image, hardware revision, test count, package/hash, runtime measurements and skipped checks. CI proves a host build; only physical hardware can prove Pi display, input, networking and recovery.

## Emulation checks

- Build the 32-bit ARM client against a Pi Zero-compatible sysroot and run pure process/contract tests under QEMU ARM user mode. Do not label an x86 host build as an ARM test.
- Boot a pinned QEMU `raspi0` fixture when available; verify start, service behavior, bounded memory and framebuffer capture. Use `raspi3ap` only as an approximate Zero 2 W class check and record that it is not a Zero 2 W emulator.
- Keep physical Zero/Zero 2 W checks for HDMI, Wi-Fi, CEC/input, power, thermals, SD failure and package recovery. Record the QEMU version, machine, kernel/OS image, architecture and limits beside each result.
