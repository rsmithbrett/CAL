#pragma once

#include <stdint.h>

/// The gesture that opens CAL's configuration menu, in one place so the two
/// binaries cannot drift on what six taps means.
///
/// CAL watches for it on its startup splash and on its own setup screens. The
/// App watches for it on a display that is already showing cards and, rather
/// than drawing the menu itself, asks CAL for it by setting cfgmenu and
/// restarting. Both arrive at the same menu, and a reader who changes the tap
/// count here changes it for both. See CAL_TOUCH_RECOVERY_MENU.md.
///
/// That last sentence was not true until kTaps and kBounceMs had a second
/// consumer. The App compared against both; CAL counted to a 6 and debounced on
/// a 100 written into RecoveryState.h, so either figure here could change and
/// CAL would carry on wanting the old gesture. Both are now read there as well,
/// and tests/recovery-menu-test.cpp drives CAL's counter and bounce window from
/// these names so a change that reaches one binary and not the other fails.
///
/// Rectangles live in RecoveryState.h rather than here, because CAL's are
/// asserted against its own menu rows and that geometry means nothing to the
/// App. What is shared is the shape of the gesture: how many taps, in what
/// corners, inside what window. RecoveryState.h reads the count, the bounce
/// window and the window length from here, and asserts its rectangles agree
/// with the corner size below, the same way App/Motion.cpp asserts its presence
/// pin agrees with PresenceInput.h.
namespace ConfigGesture {

/// Corner taps, alternating, starting top-left. Six is long enough that nobody
/// reaches it by handling the device and short enough to do with one thumb.
constexpr uint8_t kTaps = 6;

/// All six land inside this or the sequence is abandoned and starts over.
constexpr uint32_t kWindowMs = 8000;

/// Contacts closer together than this are one finger bouncing, not two taps.
constexpr uint32_t kBounceMs = 100;

/// How far in from the left or right edge a corner reaches, and how far down.
///
/// Measured from the panel's own width rather than a fixed 320, so a display
/// standing on its end has its corners where a reader sees them. The height
/// stops at 44 because that is where CAL's first menu row begins: a seventh tap
/// in the corner somebody has just tapped six times would otherwise land on Set
/// up Wi-Fi, which is the one row with no confirmation behind it.
constexpr int32_t kZoneWidth = 63;
constexpr int32_t kZoneHeight = 44;

}  // namespace ConfigGesture
