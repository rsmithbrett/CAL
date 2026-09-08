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

// How many times begin() below retries a failed mount, and how long it waits
// between attempts - found live: this used to be a single SD.begin() call
// with no retry at all, and a device that reported zero SD capacity for its
// entire uptime (assets permanently unavailable, indistinguishable in
// telemetry from "no card in the slot") turned out to still have a card
// seated, just not electrically settled the one instant setup() happened to
// call this. A real SD card can need tens to a couple hundred milliseconds
// after power-up before it reliably answers CMD0/ACMD41 - normal for the
// class of card this device uses, not a defect in any one card - and this
// firmware was giving it exactly one chance. 3 attempts, 200ms apart (600ms
// worst case, negligible against the several seconds setup() already spends
// on WiFi/SNTP before this runs) covers that settling window without
// meaningfully slowing a boot where the card was ready on the first try, the
// overwhelmingly common case this doesn't change at all.
constexpr uint8_t kMaxMountAttempts = 3;
constexpr uint32_t kMountRetryDelayMs = 200;

}  // namespace

bool begin() {
  if (gReady) {
    return true;
  }
  for (uint8_t attempt = 0; attempt < kMaxMountAttempts; ++attempt) {
    gReady = SD.begin(kChipSelectPin);
    if (gReady) {
      Log::printf("[sd] card mounted on attempt %u/%u, size=%lluMB used=%lluMB",
                  static_cast<unsigned>(attempt) + 1, static_cast<unsigned>(kMaxMountAttempts),
                  SD.cardSize() / (1024ULL * 1024ULL), SD.usedBytes() / (1024ULL * 1024ULL));
      return true;
    }
    if (attempt + 1 < kMaxMountAttempts) {
      delay(kMountRetryDelayMs);
    }
  }
  // Not a failure worth a screen. See SdStorage.h. Logged with the attempt
  // count so a genuinely empty slot (fails identically every attempt) stays
  // distinguishable from a card that needed the retry above but still lost.
  Log::printf("[sd] no card found after %u attempt(s) (or mount failed) - assets will be unavailable",
              static_cast<unsigned>(kMaxMountAttempts));
  return false;
}

bool isReady() { return gReady; }

uint64_t totalBytes() { return gReady ? SD.totalBytes() : 0; }

uint64_t usedBytes() { return gReady ? SD.usedBytes() : 0; }

}  // namespace Sd
