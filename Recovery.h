#pragma once

#include "RecoveryState.h"

namespace Recovery {

// Enable only while a setup screen is being serviced. begin() consumes any
// finger already on the glass; entering the menu needs six new taps.
void begin();
void end();
bool visible();
InputResult poll(bool gestureAllowed = true);

// Caller closes any live portal first. Persists a request, selects factory,
// then restarts. A storage/boot-selection failure stops with an explicit error.
[[noreturn]] void execute(Action action);

}  // namespace Recovery
