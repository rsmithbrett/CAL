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

      // Which SPI bus this card is actually on, reported rather than assumed.
      //
      // **This line exists because a wrong assumption about precisely this
      // cost a great deal of effort.** The board's documentation says the SD
      // card "shares SPI pins", and this codebase read that as sharing with
      // the DISPLAY - which is the entire justification for Display.cpp's
      // readFileToBuffer() pulling a whole file into RAM before decoding, so
      // that SD reads and panel writes are separated in time. That buffer is a
      // 10-24KB contiguous allocation on every draw, and it is the allocation
      // that was failing in the field.
      //
      // Checked against the actual pin assignments, the premise does not hold.
      // The panel is on HSPI_HOST at SCLK 14 / MISO 12 / MOSI 13 - see
      // LovyanGFX's _detector_Sunton_2432S028_9341_t, which also passes
      // pin_tfcard_cs = -1, the library's own assertion that no card sits on
      // the panel's host - while SD.begin() above is handed no SPIClass and so
      // takes Arduino's default SPI object on VSPI. The documentation's
      // "shared" is far more likely to mean shared with the board's general
      // SPI expansion header: both the more literal reading, and the only one
      // consistent with both subsystems having worked all along.
      //
      // That is still an inference about which object SD.begin() binds to, so
      // rather than restate the argument this logs the runtime truth: the pins
      // in force on this actual boot. A future reader comparing them against
      // the panel's figures need not trust anyone's reading of a datasheet or
      // of Arduino's defaults - including this comment's.
      Log::printf("[sd] bus pins in force: sclk=%d miso=%d mosi=%d cs=%d (panel is on HSPI at "
                  "14/12/13 - if these match, the shared-bus premise is back on the table)",
                  static_cast<int>(SCK), static_cast<int>(MISO), static_cast<int>(MOSI),
                  static_cast<int>(kChipSelectPin));
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
