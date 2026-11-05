// What a moment of the WiFi setup confirmation means. The decision only - the
// drawing and the touch controller are not here and cannot be, because
// App/Display.h needs LovyanGFX and nothing under tests/ links it.
//
// WIFI_SETUP_BUTTON_DESIGN.md is the record. The property this file exists for
// is the last block: a confirmation nobody answered must never reprovision a
// display.

#include <cassert>
#include <iostream>

#include "WifiSetupButton.h"

namespace {

using WifiSetupButton::Answer;
using WifiSetupButton::answerFor;

int checks = 0;

void check(bool condition, const char* what) {
  ++checks;
  if (!condition) {
    std::cout << "FAIL: " << what << "\n";
    std::exit(1);
  }
}

}  // namespace

int main() {
  // Nothing was tapped, which is what several hundred polls a minute look like.
  check(answerFor(false, 0, false) == Answer::NoTap, "no tap reads as NoTap");
  check(answerFor(false, 1, false) == Answer::NoTap,
        "no tap reads as NoTap whatever index is left in the struct");

  // Yes is index 0 and Cancel is index 1, in the order the labels are drawn.
  check(answerFor(true, 0, false) == Answer::Confirmed, "index 0 is Yes");
  check(answerFor(true, 1, false) == Answer::Cancelled, "index 1 is Cancel");

  // THE ONE THAT MATTERS. A display attempting WiFi is unattended by definition,
  // and a question nobody answered must not take itself back to the portal and
  // sit there broadcasting an open setup AP - which is the accident
  // CAL_TOUCH_RECOVERY_MENU.md records against the one menu row that had no
  // confirmation at all.
  check(answerFor(false, 0, true) == Answer::Cancelled, "a timeout is a Cancel");
  check(answerFor(true, 0, true) == Answer::Cancelled,
        "a timeout is a Cancel even if a tap arrives in the same moment");
  check(answerFor(true, 1, true) == Answer::Cancelled, "a timeout is a Cancel on index 1");

  // Said as the property rather than as three cases, so a fourth way of timing
  // out could not pass by nobody having written its case down.
  for (uint8_t index = 0; index < 4; ++index) {
    for (int tapped = 0; tapped < 2; ++tapped) {
      check(answerFor(tapped != 0, index, true) != Answer::Confirmed,
            "nothing that has timed out is ever Confirmed");
    }
  }

  // An index nobody draws is a Cancel rather than a Yes, for the same reason:
  // two buttons are drawn, and a third would be a bug rather than an approval.
  check(answerFor(true, 2, false) == Answer::Cancelled, "an index nobody drew is a Cancel");
  check(answerFor(true, 255, false) == Answer::Cancelled, "a wild index is a Cancel");

  std::cout << "PASS: wifi setup confirmation (" << checks << " checks)\n";
  return 0;
}
