#include "WifiSetupButton.h"

#include "Display.h"
#include "Log.h"
#include "Touch.h"

namespace WifiSetupButton {
namespace {

// Drawn with Display's own action-button pair rather than a rectangle of this
// file's own: drawActionButtons() paints and actionButtonZone() measures, and
// they are written so the two cannot disagree. That also gets the 44px floor for
// free - the row is 60px tall, which is the same button a card draws.
const String kSetUpLabel = "Set up WiFi";
const String kYesLabel = "Yes, set up WiFi";
const String kCancelLabel = "Cancel";

/// How long the confirmation waits for an answer before taking it as no.
///
/// Long enough to read two buttons and decide, short enough that a display left
/// showing a question nobody is going to answer goes back to retrying rather than
/// sitting on it. A timeout is a Cancel, never a Yes: an unattended screen must
/// not reprovision itself.
constexpr uint32_t kConfirmTimeoutMs = 30000;

void publishZone(uint8_t index, uint8_t count) {
  Touch::Rect zones[2];
  for (uint8_t i = 0; i < count; ++i) {
    int16_t x, y, w, h;
    Display::actionButtonZone(i, count, x, y, w, h);
    zones[i] = Touch::Rect{x, y, w, h};
  }
  Touch::setActionZones(zones, count);
  (void)index;
}

/// Draws the two-button question and waits for one of them.
///
/// Blocking, unlike everything else here, and the one place that is right: a
/// finger is on the glass and the household is mid-decision. Polled rather than
/// delayed in one lump so the answer lands on the tap rather than up to a second
/// later.
bool confirmed() {
  Display::showFailure("Set up WiFi again?",
                       "This restarts the display and opens its setup page.");

  const String labels[2] = {kYesLabel, kCancelLabel};
  Display::drawActionButtons(labels, 2);
  publishZone(0, 2);

  const uint32_t deadline = millis() + kConfirmTimeoutMs;
  while (millis() < deadline) {
    Touch::Tap tap;
    const bool hit = Touch::poll(tap) && tap.hit == Touch::Hit::ActionButton;
    // Index 0 is Yes and index 1 is Cancel, in the order the labels were drawn.
    // answerFor() is what decides, so this loop and the host test are reading one
    // rule rather than two copies of it.
    const Answer answer = answerFor(hit, tap.actionIndex, false);
    if (answer != Answer::NoTap) {
      const bool yes = answer == Answer::Confirmed;
      Log::printf("[wifi] setup confirmation answered %s", yes ? "yes" : "cancel");
      clear();
      return yes;
    }
    delay(50);
  }

  Log::printf("[wifi] setup confirmation timed out after %lu ms - taken as cancel",
              static_cast<unsigned long>(kConfirmTimeoutMs));
  clear();
  return answerFor(false, 0, true) == Answer::Confirmed;
}

}  // namespace

void draw() {
  const String labels[1] = {kSetUpLabel};
  Display::drawActionButtons(labels, 1);
  publishZone(0, 1);
}

void clear() { Touch::setActionZones(nullptr, 0); }

Answer poll() {
  Touch::Tap tap;
  if (!Touch::poll(tap) || tap.hit != Touch::Hit::ActionButton) {
    return Answer::NoTap;
  }

  Log::line("[wifi] setup button tapped - asking for confirmation");
  return confirmed() ? Answer::Confirmed : Answer::Cancelled;
}

}  // namespace WifiSetupButton
