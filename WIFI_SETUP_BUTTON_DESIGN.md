# A drawn button for WiFi setup, on every attempt

## Why this exists

A household whose display can't reach WiFi sees "Could not join WiFi" and a
line telling them to hold BOOT for three seconds. The gesture works. The
problem is that it's a gesture: it asks somebody standing in front of an
appliance to know which of two unlabelled-for-this-purpose buttons to hold, and
for how long, with nothing on screen they can press.

Two things make that worse than it reads.

The Panellan may have no button on GPIO0 at all. `kBootButtonPin` is 0 with no
board guard in either binary, unlike `kActionButtonPin`, which resolves to -1 on
the ESP32-S3 behind a `static_assert`. Nothing in this repository records
whether a GPIO0 button is fitted there. `BoardPanellan.h` says its pin map has
never been verified against hardware, and the one reset constant in it,
`kBoardReset`, is the display panel's reset line rather than a button.

The touchscreen recovery menu doesn't reach these screens. It's a loader
feature, and `CAL_TOUCH_RECOVERY_MENU.md` says so: application screens don't
listen for the corner gesture. So a device sitting in the App's retry loop has
the BOOT hold and nothing else.

So: a button that's drawn, labelled, and on screen every time WiFi is
attempted. Not a gesture to learn. The BOOT hold stays exactly as it is, in both
binaries, for the household who already knows it and for a panel whose touch
controller is the thing that's broken.

## Where it appears

Both binaries, because a household shouldn't need to know which one is running.
That's the same reasoning that already keeps the BOOT hold identical across the
two, recorded at `App/App.ino`'s `kBootButtonPin`.

| Binary | Screen | Drawn by |
| --- | --- | --- |
| App | "Connecting to WiFi ... (attempt n)" | `WifiJoin::attemptJoin()` through `showProgress()` |
| App | "Could not join WiFi" | `ensureWifiConnected()`'s retry loop |
| CAL | the join ladder's progress and failure screens | `Provisioning.cpp` |

It does not appear on a card, on the splash, on a firmware download, or on any
self-install screen. Those have their own owners and none of them is a WiFi
attempt.

## What a tap does

One action, with a confirmation in front of it.

Tapping draws "Set up WiFi again?" over the screen with **Yes** and **Cancel**.
Cancel redraws whatever was underneath and the attempt carries on from where it
was. Yes takes the same path the three-second BOOT hold already takes:
`BootDiag::recordRestartIntent(BootDiag::RestartCause::Reprovision)`, then
`Loader::returnToLoaderForReprovisioning()`, which doesn't return. CAL's ladder
uses its own equivalent, so the two binaries reach one portal rather than two
implementations of it.

**The confirmation is not optional, and the reason is recorded rather than
assumed.** `CAL_TOUCH_RECOVERY_MENU.md` has the scar: Set up Wi-Fi was the one
menu row with no confirmation, a single stray contact restarted a device into
its portal, and the unit sat broadcasting an open setup AP until the abandonment
timeout. A display attempting WiFi is mounted on a wall where a sleeve, a cloth
or a child reaches it, and it is attempting WiFi precisely when nobody is
watching it succeed.

Nothing else goes on this screen. No identity erase, no application reload, no
diagnostics. Those live in the recovery menu, behind a gesture, because they
aren't what somebody holding a dead display is trying to do.

## Geometry and the hit test

`Display` decides where the button is, because it's the only thing that knows
the panel's layout, and publishes the rectangle it drew. `Touch` owns the hit
test. That split already exists: `aircraftLogoZone()` publishes a zone the same
way, and `Touch::setActionZones()` plus `Touch::poll()` is the mechanism every
card button already uses.

The button is at least 44px on its shorter side. That's the floor CLAUDE.md
sets for a touch target and a 26px one has already shipped once.

**A zone outlives its screen unless somebody clears it.** `setActionZones()`
holds whatever it was last handed, and the comment on it says a draw with no
buttons clears it to zero for exactly this reason: a stale rectangle from the
previous screen firing on the current one. Both call sites here clear the zone
before they leave, and the test below drives that rather than trusting this
paragraph.

## Polling

Neither wait loop needs rebuilding. `attemptJoin()` already waits in 250ms
steps against a deadline, and `ensureWifiConnected()`'s retry already polls
GPIO0 every 100ms. The touch read goes in beside the existing work in both.

250ms is the slowest response a tap can get, on the connecting screen. That's
inside what reads as immediate for a button that then asks for confirmation, and
raising the poll rate would spend loop time on every boot for a control almost
nobody touches.

## What this does not change

The BOOT hold, on either binary. The recovery menu's corner gesture, its four
rows, and its 8-second window. The join ladder's order, its attempt count, and
its timeouts. What gets stored when the portal finishes. The wire contract, in
any direction.

No `App/` behaviour outside the two screens named above, and no card.

## How it is proven

Fail first, each one, and say so if the test was written after the code.

- A tap inside the published rectangle on the connecting screen opens the
  confirmation, and the attempt is still running underneath.
- A tap outside it does nothing, and the attempt is unaffected.
- Cancel redraws the screen that was underneath and the loop continues.
- Yes records `RestartCause::Reprovision` and calls the loader's reprovision
  entry point exactly once.
- Leaving either screen clears the action zones, so a tap in the same place on
  the next screen reaches no handler. Driven by setting a zone, leaving, and
  tapping, rather than by reading the code.
- The button's shorter side is at least 44px at both orientations and on both
  board geometries.
- The BOOT hold still reaches the same path with the button drawn, so the two
  routes don't exclude one another.

Host tests, under `tests/`, with the board geometry supplied rather than read
from hardware. These run under `ci/` and on the runner.

## What is not covered

**Nobody has put a finger on this.** Every check here is a host test against a
simulated tap. Whether the rectangle is where a thumb goes, whether the label
reads at arm's length on a wall, and whether 250ms feels immediate are physical
acceptance questions, and they belong on a bench with a device.

**The Panellan's GPIO0 button is still unknown.** This change makes that not
matter for WiFi recovery, which is the point, but it doesn't answer the question
and the pin stays unguarded in both binaries. That is its own piece of work.

**A panel whose touch controller has failed** reaches none of this. The BOOT
hold is the route there, which is why it stays.

**CAL's own screens beyond the join ladder** are untouched, so a device waiting
on enrollment still answers only the corner gesture.
