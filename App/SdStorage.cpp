#include "SdStorage.h"

#include <SD.h>
#include <esp_heap_caps.h>

#include "Log.h"

namespace Sd {
namespace {

// The same chip-select CYD-Dickey's SdCard.cpp uses on this board family
// (its `static const int SD_CS_PIN = 5;`). Not autodetected - unlike the
// panel, which LovyanGFX's LGFX_AUTODETECT identifies for us, the card slot's
// CS line is just a board wiring fact.
constexpr uint8_t kChipSelectPin = 5;

bool gReady = false;

// The mount's measured cost, retained so it can ride on telemetry instead of
// only being printed. -1 until a mount succeeds, and it STAYS -1 on a device
// with no card - see Sd::mountCostBytes() on why that is not 0.
int32_t gMountCostBytes = -1;

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

/// How many files the FAT layer keeps descriptor slots for, passed to
/// SD.begin() instead of accepting Arduino's default of 5.
///
/// **This is the largest single memory saving available to this firmware, and
/// it was found by measurement on 2026-09-11 rather than by reading.** Mounting
/// an SD card costs roughly 67KB of CONTIGUOUS 8-bit heap on this board, taken
/// at mount and held for the life of the process. Measured on device 17, same
/// firmware, same boot, before a single card was drawn:
///
///   SD card in slot     largest free 8-bit block =  10,228
///   SD card removed     largest free 8-bit block =  77,812
///
/// The consequence is not subtle. mbedTLS needs a contiguous block of
/// Http::kTlsRecordBufferBytes (16,717) for each of two record buffers, so an
/// SD-equipped device is below the floor for a NEW TLS session from the instant
/// it boots - it can never check in, never report telemetry, never receive a
/// card policy and never take a firmware update, while rendering its cards
/// perfectly off the very card that caused it. Devices 12 and 17 were both in
/// exactly that state for hours, and it is why an identical firmware image
/// installed cleanly on the two card-less devices and failed on the two with
/// cards: CAL mounts the same card before attempting a 1.3MB TLS download.
///
/// Three wrong explanations were eliminated on the way, and they are worth
/// recording because each looked right:
///   - "the image decoder retains it" - no. The measurement above was taken
///     with a policy containing no graphic card at all, so drawJpgFile() was
///     never called on either side of the comparison.
///   - "it scales with card size, so it is FAT cluster buffers" - no. A 7.81GB
///     card gives 10,228 and a 3.96GB card gives 9,204. Near-identical, so the
///     cost is fixed per mount.
///   - "card draws ratchet it down" - true but second-order. Draws take it from
///     ~10,228 to ~7,668 over minutes. The mount takes the first 67KB in one
///     step, and that step is the one that crosses the TLS floor.
///
/// 1 rather than 5 because this firmware only ever has one file open at a time:
/// Assets.cpp writes one asset then closes it, and Display.cpp streams one
/// image from SD per draw. Five slots was never a requirement, only a default,
/// and every unused slot still carries its FATFS sector buffer - FF_MAX_SS
/// bytes apiece, which is 4096 in a stock esp32 build rather than 512.
///
/// **What this does NOT claim.** It is not established that descriptor slots
/// are the whole 67KB; the SPI host's own transfer buffers and the FATFS volume
/// work area are in there too and are not tunable from here. This change is the
/// cheapest large lever available, taken because it needs one parameter and one
/// build, and it is instrumented so the answer is a fact rather than a hope -
/// begin() logs the largest contiguous block either side of the mount. If the
/// recovered figure is small, the remaining cost is in the host driver and the
/// next move is moving SD off internal 8-bit heap or off SPI, not tuning this.
constexpr int kMaxOpenFiles = 1;

/// Arduino's own defaults for the two SD.begin() parameters that sit before
/// max_files, restated here only because naming max_files positionally requires
/// passing them. Both are exactly what SD.begin(cs) would have used, so this
/// change alters the file-slot count and nothing else - worth being explicit
/// about on a mount that this device's reachability now demonstrably depends on.
constexpr uint32_t kSpiFrequencyHz = 4000000;
constexpr const char* kMountPoint = "/sd";

}  // namespace

bool begin() {
  if (gReady) {
    return true;
  }
  // Captured before the first attempt so the mount's cost is a measured delta
  // in the fleet's own logs rather than something inferred by comparing two
  // devices, or two boots, that differ in other ways too. This is the number
  // that decides whether kMaxOpenFiles was worth changing.
  const size_t largestBeforeMount = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

  for (uint8_t attempt = 0; attempt < kMaxMountAttempts; ++attempt) {
    // max_files named explicitly - see kMaxOpenFiles for the 67KB measurement
    // that forced it. The two parameters before it are Arduino's own defaults,
    // restated only because max_files is positional.
    gReady = SD.begin(kChipSelectPin, SPI, kSpiFrequencyHz, kMountPoint, kMaxOpenFiles);
    if (gReady) {
      Log::printf("[sd] card mounted on attempt %u/%u, size=%lluMB used=%lluMB",
                  static_cast<unsigned>(attempt) + 1, static_cast<unsigned>(kMaxMountAttempts),
                  SD.cardSize() / (1024ULL * 1024ULL), SD.usedBytes() / (1024ULL * 1024ULL));

      // THE MEASUREMENT, in the log, on every device, every boot.
      //
      // Mounting a card was measured costing ~67KB of contiguous 8-bit heap,
      // which is enough on its own to put a device below the 16,717 bytes
      // mbedTLS needs for one TLS record buffer - so this one line answers
      // "can this device reach the server at all this boot" before anything
      // else has had a chance to confuse the picture. Printed whether the
      // figure is good or bad: a mount that now costs little is exactly as
      // important to know as one that still costs everything, and only one of
      // those two outcomes would ever get investigated if this only logged the
      // bad case.
      const size_t largestAfterMount = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      // Retained as well as printed. Computed as a signed long first: a mount
      // that somehow LEFT more contiguous memory than it found would produce a
      // negative cost, and clamping that to 0 would hide a measurement worth
      // seeing rather than tidy one up.
      gMountCostBytes = static_cast<int32_t>(
          static_cast<long>(largestBeforeMount) - static_cast<long>(largestAfterMount));
      Log::printf("[sd] mount cost %ld bytes of the largest contiguous 8-bit block (%u -> %u) "
                  "with max_files=%d; a TLS record buffer needs 16717, so %s",
                  static_cast<long>(largestBeforeMount) - static_cast<long>(largestAfterMount),
                  static_cast<unsigned>(largestBeforeMount),
                  static_cast<unsigned>(largestAfterMount), kMaxOpenFiles,
                  largestAfterMount >= 16717
                      ? "this device can still open a new TLS session"
                      : "this device CANNOT open a new TLS session and is unreachable until it "
                        "restarts without a card, or this cost comes down further");

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

// Deliberately NOT gated on gReady the way the two above are. Those answer
// "how big is the card", which is meaningless without one. This answers "what
// did mounting cost", and -1 is the honest answer for a device that never
// mounted - whereas gating it would return 0 and claim the mount was free.
int32_t mountCostBytes() { return gMountCostBytes; }

}  // namespace Sd
