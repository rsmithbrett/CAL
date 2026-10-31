# CAL touchscreen recovery menu

## Purpose and scope

An operator at a device can open configuration from CAL's setup QR screens, and
from a window on its startup splash, without reaching the BOOT button. This adds
recovery controls to the loader; it does not add a menu to the running product
App.

Available on:

- The Wi-Fi setup QR in the captive-portal flow.
- The registration/claim QR while CAL waits for a device secret.
- The service-provided setup QR when CAL has no application to start.
- The startup splash, for a window on every boot CAL draws. See
  "The boot hold" below.

Application cards, firmware download and CAL self-install screens do not listen
for this gesture. Physical BOOT recovery remains.
A refused enrollment draws the error screen rather than a QR, and keeps asking on
the enrollment loop's own backoff rather than halting. See "A refusal waits and
asks again" below.

The menu also includes live presence and GPIO tests for both board types.
See [CAL_GPIO_DIAGNOSTICS.md](CAL_GPIO_DIAGNOSTICS.md) for the input allowlists,
graphic scales, recent-change feed, all-pin inventory and hardware checks.

## Opening and closing configuration

Tap **top-left, top-right, top-left, top-right, top-left, top-right**.
Lift your finger between taps and finish all six within **8 seconds**.

CAL uses its existing **320 × 240 landscape** orientation. The top-left zone
is `0 <= x < 64, 0 <= y < 44`; top-right is
`256 <= x < 320, 0 <= y < 44`. They are `kGestureLeft` and `kGestureRight` in
`RecoveryState.h`, and the hit test asks those rectangles rather than repeating
their numbers.

**The boundary is y=44 because that is where `kMenuRows[0]` begins.** The zones
ran to y<60 when this feature was written, which put a 48 × 16 band of row 0
inside each corner. The sixth tap opens the menu and returns before any hit
test, so that tap was safe; a seventh tap, in the corner the operator had just
tapped six times, landed on row 0. Row 0 is Set up Wi-Fi, the one row with no
confirmation, so one stray contact restarted the device into its portal and left
an open setup AP broadcasting until the abandonment timeout.

Coordinates follow the displayed image through
LovyanGFX's existing touch mapping. The zones are invisible. A wrong tap
clears the sequence; a left-corner tap can start a new sequence immediately.
Contacts less than 100 ms apart are ignored as bounce. A held finger or slide
never supplies additional taps. The sixth tap cannot also select a button.

The screen title is **Configuration**, with four full-width rows:

| Row | Label | Action |
|---|---|---|
| 1 | Set up Wi-Fi | Restart CAL and open its portal immediately. Preserve the secret, setup AP password and remembered networks. |
| 2 | Reset identity | Show confirmation. After Reset, remove the local device secret, keep Wi-Fi and restart registration recovery. |
| 3 | Reload application, or **Cancel reload** while one is pending | Show confirmation. After Reload, reinstall the server's current App even if the version matches. Preserve identity and Wi-Fi. Cancel clears a pending request and takes effect at once. |
| 4, left | Diagnostics | Open Presence sensor or GPIO monitor. These tests change no registration or network settings. |
| 4, right | Back | Return to the underlying setup screen without modifying persisted state. |

The first three rows are x=16, width=288, height=42 at y=44, 90 and 136.
The footer at y=182 splits into Diagnostics (208 pixels) and Back (72 pixels).
Confirmation buttons are 136 × 44 each, with **Reset**/**Reload** on the left at
x=16 and **Cancel** on the right at x=168. That reverses the usual order on
purpose. Back sits bottom-right on the menu at x=232, so bottom-right is where
an operator has just learned to tap to leave, and with Confirm there the pixels
that said "get me out of here" on one screen said "Reset" on the next. Cancel
now holds Back's band, and an assertion keeps it there.
Rendering and hit testing share the rectangles in `RecoveryState.h`.

Identity confirmation says an administrator **will have to** allow
re-registration, and that this menu isn't available until the device is claimed
again. Both are facts rather than possibilities. The server's
`RegisterViaMacAddressAsync` answers 409 for any device whose
`SecretRetrievedAtUtc` is set, which is every device that finished enrollment,
so a replacement secret always needs an administrator. The reset removes only the
local secret; it neither deletes the server's record nor authorizes a replacement
secret. It is not a factory reset or ownership transfer.

### A refusal waits and asks again

A refused registration draws the error screen and stays on the enrollment loop,
which re-requests a key each pass and doubles its interval to a two-minute
ceiling. So the device picks itself up on its own once an administrator presses
"Allow re-registration" on `/diag/deviceregistry`, with nobody at the device.

It halted there until 2026-10-30, and three things followed. The device could not
notice the administrator acting, so somebody had to go and power-cycle it. The
halt screen polls no input, so the recovery menu went with it and Set up Wi-Fi
and Reload became unreachable. And a device holding no secret can neither check
in nor ship debug-stream lines, so the only diagnostic channel left was a cable.
Reset identity reaches that state in one confirmed tap, which made a touchscreen
recovery feature a reason to drive to the device.

No QR is drawn on a refusal. Scanning it would open a claim page the server
answers 409 for. The cost of retrying is one request per interval on a device
that is already out of service.

Reload confirmation explains that the application downloads again and power
must stay on. It reloads the **App**, not CAL. It does not select an arbitrary
build, bypass authentication or change which release is current.

Cancel returns to Configuration. After **60 seconds without a new tap**, the
menu or confirmation closes without taking its action. Live diagnostics get
**10 minutes** instead of 60 seconds, which is generous rather than absent: a
sensor test wants no deadline a person would notice, and an unbounded screen
parks whichever loop the menu was opened from. On the enrollment QR that loop
polls for a device key, and in the captive portal it applies credentials a phone
has already submitted, so a diagnostic left open used to stop both until
somebody walked back to the device. Timers use unsigned
elapsed time, including across the `millis()` rollover. The sequence is a
convenience for a physically present operator, not an authentication boundary.
The menu displays and logs no credentials.

### Reaching it from a running App

Six taps on a device that's already showing cards open the same menu. The App
watches the same two corner zones, and on the sixth tap it sets `cfgmenu`
alongside `updreq` and restarts.

This is the way in that matters on a healthy device. The boot hold below covers
a power-on, where CAL draws the splash and listens for 1,200 ms. It can't cover
a software restart: that boot hands the panel to the App without `lcd.init()`,
so `Display::readTouch()` tests `gPanelStarted` against a panel that was never
started and returns false every time. A finger on a running device is the only
contact CAL can act on, and the App is the binary holding it.

`updreq` is what makes the menu operable rather than merely requested. It turns
`mustContactServer()` true, which turns `appOwnsThePanelThisBoot()` false, which
puts CAL back on the path that calls `Display::begin()`. The panel comes up, so
touch reads, so the rows respond. Setting `cfgmenu` on its own would open a menu
on a panel CAL never started.

CAL reads `cfgmenu` once and clears it before acting, then opens Configuration
without waiting for the gesture a second time. Clearing first means a restart
taken while the menu is open returns to a normal boot rather than back here.

Both binaries share the `cal` NVS namespace and always have, which is how
`updreq` already travels from the App to CAL. The zones and the tap count come
from `RecoveryState.h`, so the two binaries can't drift on what six taps means.

## The boot hold

`Recovery::holdBootForTouch()` runs the same opening gesture on the startup
splash. `CAL.ino` calls it once the splash is on the glass and before both the
handover and the network ladder.

It exists because every other way into this menu is a setup screen, and a
healthy device never shows one. `mustContactServer()` reads three things:
whether an update was requested, whether an App reload was requested, and
whether a bootable application is installed. With a current App and nothing
asked for, it answers no, CAL hands over about a second after the splash and no
QR is ever drawn. The devices most likely to need recovery are the ones booting
an App that misbehaves, and on those the menu couldn't be reached by touch at
all. The BOOT button was the only way in.

The call listens for `kBootListenMs`, 1,200 ms, before anybody has touched. A
tap starts a hold of `kBootHoldMs`, 10 seconds, and each further tap renews it.
Ten seconds is comfortably past the 8-second gesture window, so six taps at a
human cadence fit inside one hold. Six corner taps open Configuration and the
call returns true; `CAL.ino` then polls the menu, runs whatever action it
returns, and when the menu closes by Back or by its own idle timeout it redraws
the splash and the boot carries on from where it paused. If touches stop and the
current limit runs out, the call returns false and the boot resumes with the
splash still up, because nothing drew over it.

The cost is `kBootListenMs` on every boot CAL draws, spent ahead of the App,
whether or not anybody is there.

`kBootHoldCapMs`, 120 seconds, bounds the whole call from entry and is never
renewed. It is checked at the top of every pass, before the touch is read. A
panel reading permanently down, from a short, water or a failed controller,
would otherwise renew the hold on every pass and stop this device booting for
good, and CAL is the image that has to reach the end of its own boot for a cable
or an OTA candidate to rescue anything. The journal line at the cap names the
figure and says to suspect the touch controller.

The hold is skipped on the one boot where the App owns the loading screen. That
path leaves the panel untouched deliberately, with no `lcd.init()`, so there is
no touch to read, and bringing the panel up would be the duplicate draw that
path exists to avoid. That boot is reached from the App instead, above. See
[BOOT_SCREEN_OWNERSHIP.md](BOOT_SCREEN_OWNERSHIP.md).

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
The update-requested flag is persisted and verified before identity is removed,
so an interrupted reset cannot skip CAL on a subsequent healthy-App boot.

| Action | Changes before restart |
|---|---|
| Set up Wi-Fi | `cal/provforced=true`, `cal/updreq=true`. |
| Reset identity | Remove `cal/secret`, set `cal/provforced=false`, `cal/updreq=true`. |
| Reload application | `cal/appreload=true`, `cal/provforced=false`, `cal/updreq=true`. |

**Cancel reload is not in that table, because it does not restart.** It clears
`cal/appreload`, reads it back, and redraws the menu. `Recovery::poll()` handles
it and turns the action back into `None`, so the three call sites - each written
as "if the action is not `None`, execute it" - stay correct without knowing the
action exists. A failed clear stops with an explicit error, the same as a failed
write in `execute()`.

The one new NVS key is **`cal/appreload`**, a boolean defaulting to false.
Only CAL uses it; the App's separate Identity module needs no new API.
`mustContactServer()` checks it so a pending reload cannot take the healthy-App
shortcut even if `updreq` is later cleared.

That last property is why Cancel exists. `setAppReloadRequested(false)` runs in
one other place only, `installApplication()` on success, so a request the server
cannot satisfy never cleared itself. On a board with no current App release the
manifest reports `isConfigured` false, nothing installs, and the flag stands: every
later boot takes the full network ladder instead of the healthy-App shortcut, and
while it stands CAL skips its own update check, so the component that can leave a
device unbootable loses its own over-the-air repair route. A Panellan before its
board was declared server-side sat in exactly that state. Six taps and row 3 are
the way back for somebody at the device.

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

The controller is **20 bytes on the tested host ABI**, plus one enabled flag.
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

**The geometry is proved by `static_assert` in `RecoveryState.h`, not by that
host run.** Both corner zones against all three menu rows, the Diagnostics
footer, Back and each other; the rows against each other; Confirm against Cancel
and against Back; Cancel deliberately *overlapping* Back; the GPIO controls
against each other; the outermost edges inside 320 × 240; and the timer
orderings, including a hold that must outlast the gesture window and a cap that
must outlast the hold. They cost nothing, need no host compiler, and run in
every `arduino-cli` build of both boards, which is the point: the host check
they stand in for compared each rectangle only against its own x/y/w/h, which
holds for any rectangle and any half-open `contains()`, so every boundary
mutation passed it. That check is still in the file and still passes; the
guarantee now lives in the assertions.

`holdBootForTouch()` has no host coverage. It drives `millis()`, `delay()` and
`Display::readTouch()` in a loop, and the test's `delay()` stub throws to stand
in for the terminal error path, so the boot hold is bench work. The same is true
of the 10-minute diagnostics limit: the host test proves a live screen outlasts
the 60-second menu timeout, and nothing asserts when it finally closes. See
TEST_PLAN.md section 16.

The PR bench workflow runs these tests and builds CAL and App for the original
ESP32 and Panellan ESP32-S3, with real partition-fit checks. It uploads a bench
artifact only and does not publish a release.

## Physical acceptance checks

1. On each board, verify touch mapping at the top corners, button labels, layout
   and hit targets. Open from all three QR flows and from the splash hold.
2. Verify five taps, wrong order, slow sequences, long presses and slides do not
   open the menu. The sixth tap must not select a button, and a seventh tap in
   the same corner must do nothing at all.
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
9. Tap one corner during the splash and then stop. The boot must pause, and
   resume about ten seconds after that tap with the splash still drawn. Read the
   hold and resume lines in the journal.
10. On a device with a healthy App installed, which draws no QR at all, open
    Configuration with six corner taps from the splash. Take Back, confirm the
    splash is redrawn and the handover then completes. Repeat and let the menu
    close on its 60-second expiry instead.
11. Hold a finger on the panel from power-on and leave it there, then repeat with
    the touch lines shorted. The device must boot anyway at 120 seconds with the
    cap's line naming the figure. A unit that never boots has failed this.
12. Photograph both confirmation screens. Read the whole sentence off the glass
    and confirm it isn't ellipsized, that Reset/Reload is the left button, and
    that tapping where Back was on the menu cancels.
13. Time an untouched boot either side of this change. The splash must still
    appear within about two seconds of power, and the handover should be about
    1.2 seconds later than it was.
