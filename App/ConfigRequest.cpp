#include "ConfigRequest.h"

#include <ConfigGesture.h>

#include "Display.h"
#include "Loader.h"
#include "Log.h"

namespace ConfigRequest {
namespace {

uint8_t gStep = 0;
uint32_t gStarted = 0;
uint32_t gLastTap = 0;
bool gHaveTap = false;

}  // namespace

void noteTap(uint32_t now, int32_t x, int32_t y) {
  // Unsigned elapsed arithmetic throughout, so a display that has been up for
  // 49 days reads the same as one that has been up for a minute.
  if (gHaveTap && static_cast<uint32_t>(now - gLastTap) < ConfigGesture::kBounceMs) {
    return;
  }
  gHaveTap = true;
  gLastTap = now;

  if (gStep && static_cast<uint32_t>(now - gStarted) >= ConfigGesture::kWindowMs) {
    gStep = 0;
  }

  // THE CORNERS COME OFF THE PANEL IN USE, not off a fixed 320. A display
  // standing on its end is 240 across, and its top-right corner is at 176
  // rather than 256; reading the width here is what puts both corners where the
  // person tapping sees them. RecoveryState.h asserts CAL's own rectangles are
  // the same width and height as the figures used here.
  const bool high = y < ConfigGesture::kZoneHeight;
  const bool left = high && x < ConfigGesture::kZoneWidth;
  const bool right = high && x >= Display::screenWidth() - ConfigGesture::kZoneWidth;

  const bool expected = (gStep % 2 == 0) ? left : right;
  if (expected) {
    if (gStep == 0) gStarted = now;
    ++gStep;
  } else {
    // An unexpected left tap starts a fresh sequence rather than wasting it.
    gStep = left ? 1 : 0;
    gStarted = now;
  }

  if (gStep < ConfigGesture::kTaps) {
    return;
  }
  gStep = 0;

  Log::line("[config] six corner taps - asking CAL for the configuration menu");
  Display::showStatus("Configuration", "Restarting to open it.");
  Loader::requestConfigMenu();  // never returns
}

}  // namespace ConfigRequest
