#include "BootButton.h"

namespace BootButton {
namespace {

constexpr uint8_t kPin = 0;

/// millis() when the button was first seen down in the current press, or 0
/// while it is up. millis() can return 0 for the first millisecond after a
/// restart, so a press that starts in that millisecond is read one poll later
/// rather than treated as "no press" forever.
uint32_t gDownSince = 0;

bool gBegun = false;

}  // namespace

void begin() {
  if (gBegun) {
    return;
  }
  pinMode(kPin, INPUT_PULLUP);
  gBegun = true;
}

bool isDown() {
  begin();
  return digitalRead(kPin) == LOW;
}

void reset() { gDownSince = 0; }

bool heldFor(uint32_t holdMs) {
  begin();

  if (digitalRead(kPin) != LOW) {
    gDownSince = 0;
    return false;
  }

  const uint32_t now = millis();
  if (gDownSince == 0) {
    gDownSince = now == 0 ? 1 : now;
    return false;
  }

  // Unsigned subtraction, so a millis() rollover at 49 days gives the elapsed
  // time rather than a huge number.
  return (now - gDownSince) >= holdMs;
}

}  // namespace BootButton
