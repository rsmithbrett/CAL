#include "Touch.h"

#include "Display.h"

namespace Touch {
namespace {

bool wasTouched = false;

constexpr uint8_t kMaxZones = 4;
Rect gZones[kMaxZones];
uint8_t gZoneCount = 0;

/// 1/20 of the 320px panel width, full height, no visible chrome - the same
/// dimensions CYD-Dickey settled on for the same panel (`x < 16` and
/// `x > 304` in its loop() touch handler). Wide enough to hit with a thumb,
/// narrow enough that it never competes with card content for the middle of
/// the screen.
constexpr int32_t kEdgeZoneWidth = 16;
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
