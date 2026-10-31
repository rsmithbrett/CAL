#pragma once

#include <stdint.h>

/// Watches for the six corner taps that ask for CAL's configuration menu, on a
/// display that is already showing cards.
///
/// The App never draws that menu. On the sixth tap it hands over through
/// Loader::requestConfigMenu(), which sets the two flags and reboots into CAL.
/// This exists because the menu is otherwise out of reach on a healthy display:
/// CAL's own boot hold listens for 1.2 seconds after a power-on, and on a
/// software restart it hands the panel over without lcd.init() at all, so
/// Display::readTouch() has nothing to read. The App is the binary holding a
/// working touchscreen for all the time in between. See
/// CAL_TOUCH_RECOVERY_MENU.md.
namespace ConfigRequest {

/// Feed every tap here, with the coordinates Touch::poll reported, before the
/// caller decides what else the tap meant. Corner taps also page the deck left
/// and right, which is deliberate: paging is reversible and visible, and the
/// alternative is an invisible dead zone on a card somebody is reading.
///
/// Does not return on the sixth tap of a complete sequence - the device
/// restarts from inside it.
void noteTap(uint32_t now, int32_t x, int32_t y);

}  // namespace ConfigRequest
