#pragma once

#include <Arduino.h>

/// A drawn button on every WiFi attempt and failure screen, and the confirmation
/// in front of it.
///
/// <b>Why a button and not the BOOT hold alone.</b> The hold works, and it stays.
/// What it cannot do is tell somebody standing in front of a dead display that it
/// exists. On the fleet board it asks them to know which of two buttons to hold
/// and for how long; on the Panellan it may ask them to hold a button that is not
/// fitted, because `kBootButtonPin` is 0 with no board guard in either binary and
/// nothing in this repository records whether GPIO0 carries one there.
///
/// The touchscreen recovery menu does not reach these screens. It is a loader
/// feature and `CAL_TOUCH_RECOVERY_MENU.md` says so: application screens do not
/// listen for its corner gesture. So a device sitting in the App's retry loop has
/// the BOOT hold and nothing else, which is what this fixes.
///
/// <b>The confirmation is not optional.</b> The recovery menu's Set up Wi-Fi row
/// was the one row without one, a single stray contact restarted a device into its
/// portal, and the unit sat broadcasting an open setup AP until the abandonment
/// timeout. A display attempting WiFi is on a wall where a sleeve or a cloth
/// reaches it, and it is attempting WiFi precisely when nobody is watching.
///
/// WIFI_SETUP_BUTTON_DESIGN.md is the record.
namespace WifiSetupButton {

/// Draws the button over whatever status or failure screen is already on the
/// glass, and publishes its rectangle to `Touch` so the hit test and the drawing
/// cannot disagree about where it is.
///
/// Call it after every draw of a screen that should carry it. A screen drawn
/// without this call carries no button and, because `Touch::setActionZones()`
/// holds whatever it was last handed, would otherwise inherit this one's
/// rectangle - which is why `clear()` exists and why both callers use it.
void draw();

/// Drops the zone, so a tap in the same place on the next screen reaches nothing.
void clear();

/// What one poll of the button found.
enum class Answer : uint8_t {
  /// Nothing was tapped. The overwhelmingly common case, several times a second.
  NoTap,

  /// Tapped, and then Cancel or a confirmation nobody answered. **The caller has
  /// to redraw**, because the question was painted over whatever was underneath.
  Cancelled,

  /// Tapped and confirmed. Reprovision.
  Confirmed,
};

/// What a moment of the confirmation means, with no panel, no clock and no touch
/// controller in it.
///
/// Split out so it can be driven on a host. Everything else in this module needs
/// `Display`, which needs LovyanGFX, which is why `tests/` cannot link the rest -
/// and this is the part worth holding anyway, because it is where the decision
/// is rather than where the pixels are.
///
/// **A timeout is a Cancel and never a Confirm.** A display attempting WiFi is
/// unattended by definition, and a question nobody answered must not reprovision
/// it. That is the one property here that would hurt if it were wrong.
constexpr Answer answerFor(bool tapped, uint8_t actionIndex, bool timedOut) {
  return timedOut          ? Answer::Cancelled
         : !tapped         ? Answer::NoTap
         : actionIndex == 0 ? Answer::Confirmed
                            : Answer::Cancelled;
}

/// Polls touch once, so call it from a wait loop.
///
/// A tap draws the confirmation and blocks there until Yes, Cancel, or the idle
/// timeout - the one place this is not a poll, and deliberately: a confirmation
/// that gave up while somebody was reading it would be worse than one that waits.
///
/// **`NoTap` and `Cancelled` are separate answers and the difference is the
/// panel.** A caller that redrew on every false would repaint the screen several
/// times a second for the whole wait, which is a flicker rather than a screen.
/// Only `Cancelled` means something was drawn over.
Answer poll();

}  // namespace WifiSetupButton
