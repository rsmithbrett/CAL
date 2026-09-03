#include "SdStorage.h"

#include <SD.h>

#include "Log.h"

namespace Sd {
namespace {

// The same chip-select CYD-Dickey's SdCard.cpp uses on this board family
// (its `static const int SD_CS_PIN = 5;`). Not autodetected - unlike the
// panel, which LovyanGFX's LGFX_AUTODETECT identifies for us, the card slot's
// CS line is just a board wiring fact.
constexpr uint8_t kChipSelectPin = 5;

bool gReady = false;

}  // namespace

bool begin() {
  if (gReady) {
    return true;
  }
  gReady = SD.begin(kChipSelectPin);
  if (gReady) {
    Log::printf("[sd] card mounted, size=%lluMB used=%lluMB",
                SD.cardSize() / (1024ULL * 1024ULL), SD.usedBytes() / (1024ULL * 1024ULL));
  } else {
    // Not a failure worth a screen. See SdStorage.h.
    Log::line("[sd] no card found (or mount failed) - assets will be unavailable");
  }
  return gReady;
}

bool isReady() { return gReady; }

uint64_t totalBytes() { return gReady ? SD.totalBytes() : 0; }

uint64_t usedBytes() { return gReady ? SD.usedBytes() : 0; }

}  // namespace Sd
