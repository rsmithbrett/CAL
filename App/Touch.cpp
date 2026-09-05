#include "Touch.h"

#include "Display.h"

namespace Touch {
namespace {

bool wasTouched = false;

constexpr uint8_t kMaxZones = 4;
Rect gZones[kMaxZones];
uint8_t gZoneCount = 0;

/// Roughly 1/3 of the 320px panel width per side, full height, no visible
/// chrome. Widened from the original 16px (1/20, matching CYD-Dickey's own
/// `x < 16` / `x > 304`) after real-device feedback that a strip 5% of the
/// screen wide was too easy to miss with a thumb and made edge navigation
/// feel unresponsive - the same complaint the approved backlog suggestion
/// "Increase the size of the area on the touchscreen you use to page to the
/// left and right" describes. A wider strip can only ever compete with card
/// content in the sense of covering more of it with an invisible zone, never
/// with an actual button: Touch::poll() below checks action-button zones
/// first regardless of how much they overlap the edge strips, so widening
/// this can dim more of the reachable card content but can never swallow a
/// button press. Leaves an inner ~108px strip (320 - 2*106) for whatever a
/// card draws in the middle.
constexpr int32_t kEdgeZoneWidth = 106;
constexpr int32_t kScreenWidth = 320;

bool contains(const Rect& rect, int32_t x, int32_t y) {
  return x >= rect.x && x <= rect.x + rect.w && y >= rect.y && y <= rect.y + rect.h;
}

}  // namespace

void setActionZones(const Rect* zones, uint8_t count) {
  gZoneCount = 0;
  if (zones == nullptr) {
    return;
  }
  for (uint8_t i = 0; i < count && i < kMaxZones; ++i) {
    gZones[gZoneCount++] = zones[i];
  }
}

bool poll(Tap& tap) {
  int32_t x = 0;
  int32_t y = 0;
  const bool isTouched = Display::readTouchRaw(x, y);
  const bool justTapped = isTouched && !wasTouched;
  wasTouched = isTouched;
  if (!justTapped) {
    return false;
  }

  tap = Tap();
  tap.x = x;
  tap.y = y;

  // Buttons first - see Touch.h on why this ordering is fixed rather than
  // incidental.
  for (uint8_t i = 0; i < gZoneCount; ++i) {
    if (contains(gZones[i], x, y)) {
      tap.hit = Hit::ActionButton;
      tap.actionIndex = i;
      return true;
    }
  }

  if (x < kEdgeZoneWidth) {
    tap.hit = Hit::Reverse;
  } else if (x > kScreenWidth - kEdgeZoneWidth) {
    tap.hit = Hit::Forward;
  }
  // A tap in the middle of the card with no button under it is Hit::None -
  // reported, not swallowed, so a caller can still treat "the glass was
  // touched at all" as a signal if it ever wants to.
  return true;
}

}  // namespace Touch
