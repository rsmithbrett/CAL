#include "SdStorage.h"

// sdkconfig.h before kBoardHasCardSlot below, which reads
// CONFIG_IDF_TARGET_ESP32S3. Without it that name is undefined at the point it
// is tested, the build takes the other branch, and the result compiles clean -
// the same trap Display.cpp and CheckIn.cpp each describe at their own tops.
#include "sdkconfig.h"

#include <SD.h>
#include <esp_heap_caps.h>

#include "HeapPools.h"
#include "Http.h"
#include "Log.h"

namespace Sd {
namespace {

/// Whether this board has a card slot wired at all.
///
/// The Panellan ZX2D80CE02S-V10 does not. Every SD pin in the vendor's
/// configuration for it is -1 - see BoardPanellan.h, which transcribes them and
/// says so - so there is no slot, no card, and nothing for begin() below to
/// find. That is not the same condition as the empty slot some fleet devices
/// ship with: an empty slot can have a card put in it, and begin() retrying
/// three times over 600ms is the right response to one. On a board with no slot
/// the retries settle nothing and cost the boot three rounds of SPI bus setup
/// and card-init timeouts for an answer that is a wiring fact.
///
/// A plain `constexpr bool` with a runtime `if`, not an `#if` around the mount,
/// so both paths compile on both boards. Only the taken branch of an `#if` is
/// compiled, which is how an untaken branch rots unseen until somebody builds
/// for that board - the cost Display.cpp names at its own board gate. The
/// compiler folds the branch away either way.
///
/// Gated on CONFIG_IDF_TARGET_ESP32S3, the same macro the pin map and the
/// reported board name are gated on, so the three cannot disagree about which
/// board this binary is. Chip id is coarser than board: a second S3 panel that
/// does have a slot would need a marker of its own here, for the reason
/// CheckIn.cpp gives about kBoardWireName.
#if defined(CONFIG_IDF_TARGET_ESP32S3)
constexpr bool kBoardHasCardSlot = false;
#else
constexpr bool kBoardHasCardSlot = true;
#endif

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

  // No slot on this board, so nothing is attempted. gReady stays false and
  // gMountCostBytes stays -1, which is exactly the state every caller already
  // handles: Assets gates each of its entry points on isReady(), the splash
  // returns at the same gate, telemetry sends zero capacity and a -1 mount cost,
  // and the weather and aircraft cards never touch storage. So this board
  // behaves as a card-less fleet device behaves, and -1 still says "there was
  // nothing to mount" rather than "mounting was free".
  //
  // Returning here rather than letting SD.begin() fail three times is what the
  // gate buys. SD.begin() claims the default SPI bus and its variant pins,
  // registers a FAT volume, and runs the card-init sequence whose CMD0 waits
  // time out at 500ms each against a floating MISO - three attempts 200ms apart,
  // for a slot the board does not have.
  if (!kBoardHasCardSlot) {
    Log::line("[sd] this board has no card slot wired - every SD pin in its configuration is -1, "
              "so no mount is attempted. Assets are held in RAM for the life of each draw, the "
              "same as on a fleet device with an empty slot");
    return false;
  }

  // Captured before the first attempt so the mount's cost is a measured delta
  // in the fleet's own logs rather than something inferred by comparing two
  // devices, or two boots, that differ in other ways too. This is the number
  // that decides whether kMaxOpenFiles was worth changing.
  //
  // Internal 8-bit heap (HeapPools::kTlsCaps), because what this cost is
  // measured against is the TLS floor, and mbedTLS allocates internal. The same
  // figure as the bare capability on every device in the fleet, which registers
  // no PSRAM in its heap.
  const size_t largestBeforeMount = HeapPools::largestTlsBlockBytes();

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
      const size_t largestAfterMount = HeapPools::largestTlsBlockBytes();
      // Retained as well as printed. Computed as a signed long first: a mount
      // that somehow LEFT more contiguous memory than it found would produce a
      // negative cost, and clamping that to 0 would hide a measurement worth
      // seeing rather than tidy one up.
      gMountCostBytes = static_cast<int32_t>(
          static_cast<long>(largestBeforeMount) - static_cast<long>(largestAfterMount));
      // Http::kTlsRecordBufferBytes rather than a second copy of 16717. The two
      // were separate literals in separate files with nothing comparing them,
      // which TEST_PLAN.md tracks as an open duplication; this comparison is the
      // live one it names, so it reads the constant it is grading against.
      Log::printf("[sd] mount cost %ld bytes of the largest internal 8-bit block (%u -> %u) with "
                  "max_files=%d; a TLS record buffer needs %lu of internal, so %s",
                  static_cast<long>(largestBeforeMount) - static_cast<long>(largestAfterMount),
                  static_cast<unsigned>(largestBeforeMount),
                  static_cast<unsigned>(largestAfterMount), kMaxOpenFiles,
                  static_cast<unsigned long>(Http::kTlsRecordBufferBytes),
                  largestAfterMount >= Http::kTlsRecordBufferBytes
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
// did mounting cost", and -1 is what a device that never mounted has to report -
// whereas gating it would return 0 and claim the mount was free.
int32_t mountCostBytes() { return gMountCostBytes; }

}  // namespace Sd
