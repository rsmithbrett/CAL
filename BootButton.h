#pragma once

#include <Arduino.h>

/// The BOOT button, watched during startup.
///
/// CAL.ino owns the power-on gesture (hold to reset WiFi, hold longer to erase
/// identity). This watches the same pin during the WiFi join ladder, where the
/// household is actually standing in front of the unit waiting for it to fail.
///
/// Polled, never interrupt-driven: an ISR on this pin can fire while flash is
/// being written.
namespace BootButton {

/// Sets the pin mode. Safe to call more than once.
void begin();

/// True once the button has been continuously down for `holdMs`.
///
/// Call it repeatedly from a wait loop. The timer starts on the first call that
/// sees the button down and resets whenever it comes up, so a tap never
/// accumulates toward a hold across separate presses.
bool heldFor(uint32_t holdMs);

/// Forgets any hold in progress. Called when a caller has acted on a hold, so
/// the next caller starts from zero rather than inheriting a finished gesture.
void reset();

/// Whether the button is down right now, with no timing.
bool isDown();

}  // namespace BootButton
