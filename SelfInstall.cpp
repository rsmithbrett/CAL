#include "SelfInstall.h"

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>

#include "Display.h"
#include "Identity.h"
#include "Journal.h"

namespace SelfInstall {
namespace {

/// 4KB, which is the flash sector size on this part. Copying in sector units
/// means an interrupted copy leaves whole sectors written and whole sectors
/// erased, never a half-written one - and it is the largest block that costs no
/// contiguous heap worth worrying about.
constexpr size_t kBlock = 4096;

/// Three, then stop. Bounds the one failure this design cannot otherwise escape:
/// a copy that keeps failing on hardware rather than on interruption. See
/// Identity::calCopyAttempts.
constexpr uint8_t kMaxCopyAttempts = 3;

const esp_partition_t* factoryPartition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                  ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
}

const esp_partition_t* otaPartition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                  ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
}

String toHex(const uint8_t* bytes, size_t length) {
  static const char* digits = "0123456789abcdef";
  String out;
  out.reserve(length * 2);
  for (size_t i = 0; i < length; ++i) {
    out += digits[bytes[i] >> 4];
    out += digits[bytes[i] & 0x0F];
  }
  return out;
}

/// SHA-256 of `length` bytes read from `part`, streamed a block at a time.
///
/// Streamed rather than buffered because the whole point is an image larger than
/// any allocation this device can make - 1.3MB against a 110KB largest
/// contiguous block. mbedtls is already linked for TLS, so this costs no new
/// dependency.
bool hashPartition(const esp_partition_t* part, uint32_t length, String& hexOut) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  if (mbedtls_sha256_starts(&ctx, 0) != 0) {
    mbedtls_sha256_free(&ctx);
    return false;
  }

  uint8_t* buffer = static_cast<uint8_t*>(malloc(kBlock));
  if (buffer == nullptr) {
    mbedtls_sha256_free(&ctx);
    Journal::line("[selfinstall] could not allocate a 4KB block to hash with");
    return false;
  }

  bool ok = true;
  for (uint32_t offset = 0; offset < length;) {
    const size_t want = (length - offset) < kBlock ? (length - offset) : kBlock;
    if (esp_partition_read(part, offset, buffer, want) != ESP_OK ||
        mbedtls_sha256_update(&ctx, buffer, want) != 0) {
      ok = false;
      break;
    }
    offset += want;
  }

  uint8_t digest[32] = {0};
  if (ok) {
    ok = mbedtls_sha256_finish(&ctx, digest) == 0;
  }
  free(buffer);
  mbedtls_sha256_free(&ctx);
  if (ok) {
    hexOut = toHex(digest, sizeof(digest));
  }
  return ok;
}

/// Erases `factory` and copies `length` bytes into it from `ota_0`, comparing
/// every block back out of flash as it goes.
///
/// **`otadata` is not touched here, and that is the design.** From the erase
/// until the caller has verified the result, `factory` is invalid - which is
/// survivable only because `otadata` still names `ota_0`, which still holds this
/// candidate. A power cut anywhere in this function boots the same candidate
/// again, which retries. See CAL_OTA_DESIGN.md section 10's interruption table.
bool copyOtaIntoFactory(const esp_partition_t* src, const esp_partition_t* dst,
                        uint32_t length) {
  Journal::printf("[selfinstall] erasing factory at 0x%06lx (%lu bytes) - from here until "
                  "the verify below, factory is INVALID and otadata still names ota_0, "
                  "which is what makes an interruption survivable",
                  static_cast<unsigned long>(dst->address),
                  static_cast<unsigned long>(dst->size));
  if (esp_partition_erase_range(dst, 0, dst->size) != ESP_OK) {
    Journal::line("[selfinstall] factory erase FAILED - nothing was written, otadata is "
                  "untouched, and the next boot is this same candidate");
    return false;
  }

  uint8_t* buffer = static_cast<uint8_t*>(malloc(kBlock));
  uint8_t* readBack = static_cast<uint8_t*>(malloc(kBlock));
  if (buffer == nullptr || readBack == nullptr) {
    free(buffer);
    free(readBack);
    Journal::line("[selfinstall] could not allocate two 4KB blocks for the copy");
    return false;
  }

  bool ok = true;
  uint8_t lastPercent = 255;
  for (uint32_t offset = 0; offset < length;) {
    const size_t want = (length - offset) < kBlock ? (length - offset) : kBlock;
    if (esp_partition_read(src, offset, buffer, want) != ESP_OK) {
      Journal::printf("[selfinstall] read from ota_0 failed at offset %lu",
                      static_cast<unsigned long>(offset));
      ok = false;
      break;
    }
    if (esp_partition_write(dst, offset, buffer, want) != ESP_OK) {
      // The first place this can fail is the interesting one: writing factory
      // from firmware had never executed anywhere before this code shipped.
      Journal::printf("[selfinstall] write to factory failed at offset %lu - this is the "
                      "operation nothing had ever exercised before",
                      static_cast<unsigned long>(offset));
      ok = false;
      break;
    }
    // Compared out of FLASH, not against the buffer still in RAM. A write that
    // silently did nothing would pass any check that trusted its own memory.
    if (esp_partition_read(dst, offset, readBack, want) != ESP_OK ||
        memcmp(buffer, readBack, want) != 0) {
      Journal::printf("[selfinstall] factory does not read back what was written at "
                      "offset %lu - stopping with otadata untouched",
                      static_cast<unsigned long>(offset));
      ok = false;
      break;
    }
    offset += want;

    const uint8_t percent = static_cast<uint8_t>((offset * 100UL) / length);
    if (percent / 10 != lastPercent / 10) {
      lastPercent = percent;
      Journal::printf("[selfinstall] %u%% - %lu of %lu bytes copied into factory", percent,
                      static_cast<unsigned long>(offset),
                      static_cast<unsigned long>(length));
      Display::showStatus("Updating", "Do not unplug");
    }
  }

  free(buffer);
  free(readBack);
  return ok;
}

}  // namespace

bool runningAsCandidate() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* ota = otaPartition();
  return running != nullptr && ota != nullptr && running->address == ota->address;
}

void cleanUpAfterCandidate() {
  if (!Identity::calCleanupPending()) {
    return;
  }

  const esp_partition_t* ota = otaPartition();
  if (ota == nullptr) {
    Identity::setCalCleanupPending(false);
    return;
  }

  // Only the first sector, which holds the image header. That is enough to make
  // esp_image reject it, and it is 4KB of erase rather than 2.4MB - this runs on
  // the boot after an update, and a device should not spend a minute erasing a
  // partition that is about to be overwritten by the App download anyway.
  Journal::line("[selfinstall] previous boot was a candidate that finished - erasing "
                "ota_0's image header so the ladder does not mistake a CAL sitting there "
                "for an installable App and hand over to it, which would be another "
                "candidate and a boot loop");
  if (esp_partition_erase_range(ota, 0, 4096) != ESP_OK) {
    // Deliberately not fatal. The consequence is a hand-over to the candidate in
    // ota_0, which will find factory already matching its own hash, skip the
    // copy, and clear this marker itself. Slower, still terminating.
    Journal::line("[selfinstall] could not erase ota_0's header - the next boot may hand "
                  "over to the candidate again, which will find factory already correct "
                  "and skip its copy");
    return;
  }
  Identity::setCalCleanupPending(false);
  Identity::clearCalCandidate();
  Journal::line("[selfinstall] ota_0 header cleared and candidate state forgotten - this "
                "device is back to an ordinary CAL with no App installed, and the ladder "
                "below will download one");
}

bool applyCandidate() {
  const esp_partition_t* src = otaPartition();
  const esp_partition_t* dst = factoryPartition();
  if (src == nullptr || dst == nullptr) {
    Journal::line("[selfinstall] refusing: this device's table has no factory or no ota_0");
    return false;
  }

  const uint32_t length = Identity::calCandidateSize();
  const String expected = Identity::calCandidateSha();
  if (length == 0 || expected.length() != 64) {
    // A CAL in ota_0 with nothing recorded about it is not a candidate anybody
    // created on purpose - most likely a bench write, which is exactly how this
    // path was first exercised on 2026-09-16. Refuse rather than guess a length.
    Journal::printf("[selfinstall] refusing: CAL is running from ota_0 but nvs records no "
                    "candidate (size=%lu shaLen=%u). Nothing here knows how long this "
                    "image is, and factory is smaller than the partition it sits in",
                    static_cast<unsigned long>(length),
                    static_cast<unsigned>(expected.length()));
    return false;
  }

  if (length > dst->size) {
    Journal::printf("[selfinstall] refusing: candidate is %lu bytes and factory holds %lu. "
                    "factory is the SMALLER partition, which is the one direction this "
                    "bites - see the field ceiling check in ci/build-firmware.sh",
                    static_cast<unsigned long>(length),
                    static_cast<unsigned long>(dst->size));
    return false;
  }

  const uint8_t attempts = Identity::calCopyAttempts();
  if (attempts >= kMaxCopyAttempts) {
    Journal::printf("[selfinstall] refusing: %u copy attempts have already failed. Stopping "
                    "rather than looping - this is the one state in this design that needs "
                    "a cable, and it is reachable only by hardware failure",
                    static_cast<unsigned>(attempts));
    Display::showFailure("Cannot finish update",
                         "Contact support - this device needs servicing.");
    return false;
  }

  // VERIFY OURSELVES BEFORE WRITING ANYTHING. A candidate that does not match
  // what phase 1 recorded is not copied into the recovery partition. This is the
  // cheapest of the three checks and the one that protects the most.
  String actual;
  if (!hashPartition(src, length, actual)) {
    Journal::line("[selfinstall] refusing: could not hash ota_0 to check this candidate "
                  "against what was recorded");
    return false;
  }
  if (!actual.equalsIgnoreCase(expected)) {
    Journal::printf("[selfinstall] refusing: ota_0 does not match the recorded candidate. "
                    "expected %s, read %s. Nothing was written to factory",
                    expected.c_str(), actual.c_str());
    Identity::clearCalCandidate();
    return false;
  }
  Journal::printf("[selfinstall] candidate verified in ota_0: %lu bytes, sha256 %s",
                  static_cast<unsigned long>(length), actual.c_str());

  // IDEMPOTENT RE-ENTRY. A power cut after a completed copy but before otadata
  // moved boots this candidate again. Hashing factory first means that boot skips
  // the copy instead of erasing a partition that was already correct - which is
  // both faster and one less window in which factory is invalid.
  String inFactory;
  if (hashPartition(dst, length, inFactory) && inFactory.equalsIgnoreCase(expected)) {
    Journal::line("[selfinstall] factory already holds this exact candidate - a previous "
                  "boot completed the copy and was interrupted before it could repoint "
                  "otadata, so the copy is skipped and only the handover remains");
  } else {
    Identity::recordCalCopyAttempt();
    Journal::printf("[selfinstall] copy attempt %u of %u",
                    static_cast<unsigned>(Identity::calCopyAttempts()),
                    static_cast<unsigned>(kMaxCopyAttempts));
    Display::showStatus("Updating", "Do not unplug");
    if (!copyOtaIntoFactory(src, dst, length)) {
      Journal::line("[selfinstall] copy FAILED - otadata is untouched, so the next boot is "
                    "this same candidate and it will try again");
      return false;
    }

    // Re-read the whole of factory from flash. The block-by-block compare above
    // catches a bad write; this catches a bad plan - a wrong length, an offset
    // error, anything that produced a self-consistent but wrong image.
    String written;
    if (!hashPartition(dst, length, written) || !written.equalsIgnoreCase(expected)) {
      Journal::printf("[selfinstall] factory does not hash to the candidate after copying. "
                      "expected %s, read %s. otadata untouched - the next boot is this "
                      "candidate again",
                      expected.c_str(), written.c_str());
      return false;
    }
    Journal::line("[selfinstall] factory verified byte-for-byte and by hash");
  }

  // ORDER MATTERS AND THIS IS THE LAST CHANCE TO GET IT WRONG. The marker is set
  // BEFORE otadata is erased, so a cut between the two still leaves the next boot
  // able to clean up. Erasing otadata is the commit: from the instant it
  // succeeds, the next boot is factory, and factory is verified.
  Identity::setCalCleanupPending(true);

  const esp_partition_t* otadata =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA,
                               nullptr);
  if (otadata == nullptr || esp_partition_erase_range(otadata, 0, otadata->size) != ESP_OK) {
    Journal::line("[selfinstall] could not erase otadata - factory holds the new CAL and is "
                  "verified, but the boot pointer still names ota_0. The next boot is this "
                  "candidate, which will find factory correct and try this step again");
    return false;
  }

  Journal::line("[selfinstall] otadata cleared - the next boot runs the new CAL from "
                "factory, which will erase ota_0's header and download an App. Restarting");
  delay(100);
  esp_restart();
  return true;  // not reached
}

}  // namespace SelfInstall
