# CAL touchscreen recovery menu

## Purpose and scope

An operator at a device can open configuration from CAL's setup QR screens
without reaching the BOOT button. This adds recovery controls to the loader;
it does not add a menu to the running product App.

Available on:

- The Wi-Fi setup QR in the captive-portal flow.
- The registration/claim QR while CAL waits for a device secret.
- The service-provided setup QR when CAL has no application to start.

The startup splash, application cards, firmware download and CAL self-install
screens do not listen for this gesture. Physical BOOT recovery remains.
A refused enrollment is still the existing terminal error screen, not a QR.

## Opening and closing configuration

Tap **top-left, top-right, top-left, top-right, top-left, top-right**.
Lift your finger between taps and finish all six within **8 seconds**.

CAL uses its existing **320 × 240 landscape** orientation. The top-left zone
is `0 <= x < 64, 0 <= y < 60`; top-right is
`256 <= x < 320, 0 <= y < 60`. Coordinates follow the displayed image through
LovyanGFX's existing touch mapping. The zones are invisible. A wrong tap
clears the sequence; a left-corner tap can start a new sequence immediately.
Contacts less than 100 ms apart are ignored as bounce. A held finger or slide
never supplies additional taps. The sixth tap cannot also select a button.

The screen title is **Configuration**, with four full-width rows:

| Row | Label | Action |
|---|---|---|
| 1 | Set up Wi-Fi | Restart CAL and open its portal immediately. Preserve the secret, setup AP password and remembered networks. |
| 2 | Reset identity | Show confirmation. After Reset, remove the local device secret, keep Wi-Fi and restart registration recovery. |
| 3 | Reload application | Show confirmation. After Reload, reinstall the server's current App even if the version matches. Preserve identity and Wi-Fi. |
| 4 | Back | Return to the underlying setup screen without modifying persisted state. |

Rows are at x=16, width=288, height=42, with y=44, 90, 136 and 182.
Confirmation buttons are **Cancel** and **Reset**/**Reload**, 136 × 44 each.
Rendering and hit testing share the rectangles in `RecoveryState.h`.

Identity confirmation explains that an administrator may need to allow
re-registration. It removes only the local secret; it neither deletes the
server's record nor authorizes a replacement secret. It is not a factory reset
or ownership transfer.

Reload confirmation explains that the application downloads again and power
must stay on. It reloads the **App**, not CAL. It does not select an arbitrary
build, bypass authentication or change which release is current.

Cancel returns to Configuration. After **60 seconds without a new tap**, the
menu or confirmation closes without taking its action. Timers use unsigned
elapsed time, including across the `millis()` rollover. The sequence is a
convenience for a physically present operator, not an authentication boundary.
The menu displays and logs no credentials.

## Portal and enrollment behavior

The captive portal keeps processing HTTP and DNS while configuration is open.
Wi-Fi events update the underlying stage but cannot repaint over the menu.
An ordinary tap outside the menu still restores the QR. The opening sequence
counts only while the QR is the underlying portal stage; if connected status
is showing, the first ordinary tap restores the QR.

A phone can submit credentials while configuration is open. The save continues
normally, but CAL waits for Back or menu expiry before joining. Selecting an
action closes HTTP, DNS and the setup AP before executing it. Closing the menu
redraws the underlying stage even if unchanged. Time spent in configuration
refreshes the portal's abandonment timer.

Enrollment waits service touch and the serial journal every 10 ms between
requests. Server polling pauses while configuration is visible. A synchronous
enrollment request already in flight can temporarily delay touch handling.
This change adds no touch task, queue or separate framebuffer.

## Persistent requests and forced reload

`Recovery::execute()` verifies factory boot selection, persists the request,
reads relevant values back, records the existing restart cause and restarts.
A boot-selection or persistence failure stops with an explicit error.

| Action | Changes before restart |
|---|---|
| Set up Wi-Fi | `cal/provforced=true`, `cal/updreq=true`. |
| Reset identity | Remove `cal/secret`, set `cal/provforced=false`, `cal/updreq=true`. |
| Reload application | `cal/appreload=true`, `cal/provforced=false`, `cal/updreq=true`. |

The one new NVS key is **`cal/appreload`**, a boolean defaulting to false.
Only CAL uses it; the App's separate Identity module needs no new API.
`mustContactServer()` checks it so a pending reload cannot take the healthy-App
shortcut even if `updreq` is later cleared.

An explicit reload skips the optional CAL-update check and goes to the App
manifest. A valid, configured manifest is still required. The existing installer
retains its size/hash checks and single-slot behavior. The flag clears **only
after a successful App installation**, alongside the update-requested flag and
boot-attempt reset. An unavailable/unconfigured manifest, unreachable service
or failed installation leaves the request pending for a later boot. No installed
version metadata is erased to force a reload.

This does not promise the old App survives a failed download. There is only one
OTA slot: once writing starts it may contain an incomplete image. Existing
fallback starts an App only if still considered bootable; otherwise CAL shows
its normal error and a restart retries recovery.

Wi-Fi never clears the secret. The App's separate BOOT-gesture fix is
[PR #8](https://github.com/rsmithbrett/CAL/pull/8). This CAL-menu PR is independent
and does not duplicate or merge that change.

## Memory, build and deployment

The controller is **16 bytes on the tested host ABI**, plus one enabled flag.
The gesture uses fixed fields, no dynamic allocation, no additional task/stack,
no new driver and no framebuffer. Drawing reuses the LCD and text helpers;
temporary Arduino Strings can be allocated for confirmation text by those
existing helpers. This is not a measured device-heap figure.

The earlier 3–10 KB flash and under-1 KB temporary-RAM figures are planning
estimates, not release measurements. Measure actual flash/static-RAM growth
with matching baseline and changed builds for each board. The existing CI gate
checks factory fit against the compiled partition table rather than Arduino's
generic ceiling.

This changes CAL; installing only the product App does not add the menu.
CAL OTA already exists through a candidate in `ota_0` and factory-copy
trampoline; see [CAL_OTA_DESIGN.md](CAL_OTA_DESIGN.md). Capable devices can use
that established release/registration process. Older loaders without that
mechanism need a wired update. No partition-table change, release publication
or current-build registration is part of this PR.

## Automated verification

Run `bash ci/test-recovery-menu.sh`. It compiles production `Recovery.cpp` with
hardware/NVS/display stubs and tests the actual state machine and manifest
install decision:

- Six taps, wrong order, outside taps, five taps, timeout and disabled entry.
- Held/dragged fingers, bounce and the finger present when setup begins.
- Both confirmations, Cancel, Back, inactivity expiry and timer rollover.
- Shared button boundaries and production menu-render handoff.
- Each action's persisted intent, secret preservation/removal and restart cause.
- Missing factory and failed persistence without a successful restart.
- Same-version forced reload, normal update/missing App, invalid manifest and
  no configured release.

The PR bench workflow runs these tests and builds CAL and App for the original
ESP32 and Panellan ESP32-S3, with real partition-fit checks. It uploads a bench
artifact only and does not publish a release.

## Physical acceptance checks

1. On each board, verify touch mapping at the top corners, button labels, layout
   and hit targets. Open from all three QR flows.
2. Verify five taps, wrong order, slow sequences, long presses and slides do not
   open the menu. The sixth tap must not select a button.
3. Open during Wi-Fi setup, connect/disconnect a phone and submit its form while
   the menu is open. Confirm the menu stays visible, portal requests continue
   and Back/expiry resumes setup without a blank screen.
4. Add a second Wi-Fi network. Confirm the secret and server record stay the
   same and other remembered sites remain available.
5. Cancel identity reset and verify nothing changes. Confirm reset on a bench
   device, check Wi-Fi remains and exercise administrator re-registration.
   Never print the secret to verify it.
6. Reload an App already at the current version. Observe an actual download,
   checksum verification, installation and boot. Check the flag clears and the
   next normal boot skips CAL's network ladder.
7. On a USB-recoverable unit, test unavailable service, no current release, bad
   hash and interrupted download. Check the request stays pending, factory
   remains available and recovery can retry. Reload must not update CAL first.
8. Measure firmware sizes and free/largest internal heap before/after opening
   and closing configuration. Host tests do not replace hardware QA.
