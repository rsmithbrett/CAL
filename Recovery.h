#pragma once

#include "RecoveryState.h"

namespace Recovery {

// Enable only while a setup screen is being serviced. begin() consumes any
// finger already on the glass; entering the menu needs six new taps.
void begin();
void end();
bool visible();
InputResult poll(bool gestureAllowed = true);

/// Holds the boot sequence while somebody keeps touching the panel, running the
/// opening gesture as it waits. Call it after the splash is drawn and before the
/// handover or the network ladder.
///
/// WHY. Every other way into this menu is a setup screen, and a healthy device
/// never shows one: with a claimed secret and a current App, mustContactServer()
/// is false and CAL hands over about a second after the splash. So on the
/// devices most likely to need recovery - the ones booting an App that
/// misbehaves - the menu could not be reached by touch at all, and the BOOT
/// button was the only way in. This opens a window on every boot CAL draws.
///
/// WHAT IT COSTS. kBootListenMs on every boot, spent before the App starts,
/// whether or not anybody is there. That is the price of the window.
///
/// Returns true when the menu is open and the caller should go on polling it,
/// false to carry on booting. It cannot block forever - kBootHoldCapMs bounds
/// the whole call, because a panel reading permanently down would otherwise
/// stop every boot of the one image that cannot be replaced except over the air.
bool holdBootForTouch();

// Caller closes any live portal first. Persists a request, selects factory,
// then restarts. A storage/boot-selection failure stops with an explicit error.
[[noreturn]] void execute(Action action);

}  // namespace Recovery
