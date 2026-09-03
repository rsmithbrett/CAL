#include "Touch.h"

#include "Display.h"

namespace Touch {
namespace {

bool wasTouched = false;

}  // namespace

bool wasTapped() {
  int32_t x, y;
  const bool isTouched = Display::readTouchRaw(x, y);
  const bool justTapped = isTouched && !wasTouched;
  wasTouched = isTouched;
  return justTapped;
}

}  // namespace Touch
