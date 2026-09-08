#include "SdTest.h"

#include <SD.h>
#include <mbedtls/sha256.h>
#include <string.h>

#include "Log.h"

namespace SdTest {
namespace {

// Same chip-select CYD-Dickey and App/SdStorage.cpp both use on this board
// family. Not autodetected - a board wiring fact, same as App's own copy of
// this constant.
constexpr uint8_t kChipSelectPin = 5;

constexpr const char* kFormatTestDir = "/selftest_fmt";
constexpr const char* kIoTestDir = "/selftest_io";

/// Fills `buffer` with a deterministic, position-and-seed-dependent pattern
/// - not all-zeros or all-0xFF, both of which a marginal card or a stuck
/// data line can produce by accident and have it look like a match. Mirrors
/// the intent (not the exact bytes) of App/Assets.cpp's SHA-256-verified
/// write path.
void fillPattern(uint8_t* buffer, size_t len, size_t offset, uint8_t seed) {
  for (size_t i = 0; i < len; ++i) {
    buffer[i] = static_cast<uint8_t>(((offset + i) * 31 + seed) % 251);
  }
}

/// Streams `size` bytes of fillPattern() into `path`, timing the write and
/// hashing it as it goes - the same "hash while writing" technique
/// App/Assets.cpp's fetchToCard() uses for exactly the same reason: proving
/// what actually landed on the card, not just that write() was called.
bool writePatternFile(const String& path, size_t size, uint8_t seed, uint32_t& elapsedMs,
                      uint8_t digestOut[32]) {
  File f = SD.open(path, FILE_WRITE);
  if (!f) {
    return false;
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);

  uint8_t buffer[512];
  size_t written = 0;
  const uint32_t startMs = millis();
  bool ok = true;
  while (written < size) {
    const size_t chunk = (size - written) < sizeof(buffer) ? (size - written) : sizeof(buffer);
    fillPattern(buffer, chunk, written, seed);
    if (f.write(buffer, chunk) != chunk) {
      ok = false;
      break;
    }
    mbedtls_sha256_update(&sha, buffer, chunk);
    written += chunk;
  }
  elapsedMs = millis() - startMs;
  f.close();
  mbedtls_sha256_finish(&sha, digestOut);
  mbedtls_sha256_free(&sha);
  return ok && written == size;
}

/// Reads `path` back and hashes what was actually read - compared by the
/// caller against writePatternFile()'s digest. A size mismatch alone fails
/// this before the hash comparison even runs, the same two-layer check
/// App/Assets.cpp's fetchToCard() uses (size first, then hash) so a short
/// read is never mistaken for a hash coincidence.
bool readAndHashFile(const String& path, size_t expectedSize, uint32_t& elapsedMs,
                     uint8_t digestOut[32], size_t& totalReadOut) {
  File f = SD.open(path, FILE_READ);
  if (!f) {
    totalReadOut = 0;
    return false;
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);

  uint8_t buffer[512];
  size_t total = 0;
  const uint32_t startMs = millis();
  while (f.available()) {
    const int r = f.read(buffer, sizeof(buffer));
    if (r <= 0) {
      break;
    }
    mbedtls_sha256_update(&sha, buffer, r);
    total += static_cast<size_t>(r);
  }
  elapsedMs = millis() - startMs;
  f.close();
  mbedtls_sha256_finish(&sha, digestOut);
  mbedtls_sha256_free(&sha);
  totalReadOut = total;
  return total == expectedSize;
}

/// Category 2: create the test directory, then write/read-verify/delete
/// three files sized to match real assets from tonight's incident (a few
/// KB, ~34KB, ~80KB) - each step's own pass/fail recorded rather than one
/// aggregate, per the explicit brief this sketch was built against. There
/// is no low-level "format" call in the Arduino SD library to invoke - this
/// is the honest substitute: a full create/write/verify/delete round trip
/// against real files, which is what a "the card can be wiped and reused"
/// claim actually depends on being true.
FormatResult runFormatTest() {
  FormatResult result;

  // (a) Report the mount honestly, before anything else - a failed mount
  // is a FAILED result here, not a silent no-op success. Single attempt,
  // no retry-and-hide: App/SdStorage.cpp's 3-attempt retry exists to smooth
  // over a card that hadn't electrically settled yet, which is a real and
  // legitimate accommodation for normal operation - but this diagnostic
  // tool's whole job is to say what actually happened on this exact call,
  // not to paper over a slow card the same way production code reasonably
  // does.
  result.mountedBefore = SD.begin(kChipSelectPin);
  if (!result.mountedBefore) {
    result.errorDetail = "SD.begin() returned false - no card detected or mount failed";
    result.mountedAfter = false;
    Log::line("[sdtest] format test: SD.begin() failed, mount is the failure");
    return result;
  }

  bool allStepsOk = true;
  String failures;

  const bool mkdirOk = SD.exists(kFormatTestDir) || SD.mkdir(kFormatTestDir);
  Log::printf("[sdtest] format test: mkdir %s -> %s", kFormatTestDir, mkdirOk ? "ok" : "FAILED");
  if (!mkdirOk) {
    allStepsOk = false;
    failures += "mkdir failed; ";
  }

  struct FileSpec {
    const char* name;
    size_t size;
    uint8_t seed;
  };
  const FileSpec files[] = {
      {"test_2048.bin", 2 * 1024, 0x11},
      {"test_34816.bin", 34 * 1024, 0x22},
      {"test_80000.bin", 80000, 0x33},
  };

  if (mkdirOk) {
    for (const FileSpec& spec : files) {
      const String path = String(kFormatTestDir) + "/" + spec.name;
      uint32_t writeMs = 0;
      uint8_t writeDigest[32];
      const bool writeOk = writePatternFile(path, spec.size, spec.seed, writeMs, writeDigest);
      Log::printf("[sdtest] format test: write %s (%u bytes) -> %s (%lums)", spec.name,
                  static_cast<unsigned>(spec.size), writeOk ? "ok" : "FAILED",
                  static_cast<unsigned long>(writeMs));
      if (!writeOk) {
        allStepsOk = false;
        failures += String(spec.name) + " write failed; ";
        continue;
      }

      uint32_t readMs = 0;
      uint8_t readDigest[32];
      size_t totalRead = 0;
      const bool readOk = readAndHashFile(path, spec.size, readMs, readDigest, totalRead);
      const bool hashOk = readOk && memcmp(readDigest, writeDigest, 32) == 0;
      Log::printf("[sdtest] format test: read+verify %s -> %s (%lums, %u bytes)", spec.name,
                  hashOk ? "ok" : "FAILED", static_cast<unsigned long>(readMs),
                  static_cast<unsigned>(totalRead));
      if (!hashOk) {
        allStepsOk = false;
        failures += String(spec.name) + " byte-for-byte verify failed; ";
      }

      const bool removeCalled = SD.remove(path);
      const bool actuallyGone = !SD.exists(path);
      Log::printf("[sdtest] format test: delete %s -> remove()=%d, exists()=%d (gone=%d)",
                  spec.name, removeCalled, SD.exists(path), actuallyGone);
      if (!actuallyGone) {
        allStepsOk = false;
        failures += String(spec.name) + " still exists after delete; ";
      }
    }
    // Best-effort cleanup of the directory itself - not treated as a
    // pass/fail step of its own since a non-empty or already-removed
    // directory is not a hardware fault.
    SD.rmdir(kFormatTestDir);
  }

  result.formatSucceeded = allStepsOk;
  result.errorDetail = failures;
  // (b) card size/used bytes, once we know the mount is real.
  Log::printf("[sdtest] format test: card size=%lluMB used=%lluMB",
              SD.cardSize() / (1024ULL * 1024ULL), SD.usedBytes() / (1024ULL * 1024ULL));

  // Confirm the mount is still alive after the round trip, rather than
  // assuming it - a card that drops out mid-test is exactly the kind of
  // marginal behaviour this tool exists to catch.
  File root = SD.open("/");
  result.mountedAfter = static_cast<bool>(root);
  if (root) {
    root.close();
  }
  return result;
}

/// Category 3: write/read/verify a range of sizes representative of the
/// real asset catalog's range (2KB/34KB/80KB/150KB - the 34KB/80KB points
/// are the exact sizes implicated in tonight's read-buffer fragmentation
/// bug, see App/Display.cpp's own remarks).
void runFileIoTest(Results& results) {
  const uint32_t sizes[] = {2048, 34816, 80000, 150000};
  for (uint8_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]) && i < kMaxFileIoResults; ++i) {
    FileIoResult& entry = results.fileIo[results.fileIoCount++];
    entry.sizeBytes = sizes[i];

    if (!results.format.mountedBefore) {
      entry.errorDetail = "card not mounted - skipped";
      Log::printf("[sdtest] fileio %u bytes: skipped, card not mounted", sizes[i]);
      continue;
    }

    if (!SD.exists(kIoTestDir)) {
      SD.mkdir(kIoTestDir);
    }
    const String path = String(kIoTestDir) + "/file_" + String(sizes[i]) + ".bin";
    const uint8_t seed = static_cast<uint8_t>(0x50 + i);

    uint8_t writeDigest[32];
    entry.writeSucceeded =
        writePatternFile(path, sizes[i], seed, entry.writeMs, writeDigest);
    Log::printf("[sdtest] fileio %u bytes: write -> %s (%lums)", sizes[i],
                entry.writeSucceeded ? "ok" : "FAILED",
                static_cast<unsigned long>(entry.writeMs));
    if (!entry.writeSucceeded) {
      entry.errorDetail = "write failed";
      SD.remove(path);
      continue;
    }

    uint8_t readDigest[32];
    size_t totalRead = 0;
    entry.readSucceeded = readAndHashFile(path, sizes[i], entry.readMs, readDigest, totalRead);
    Log::printf("[sdtest] fileio %u bytes: read -> %s (%lums, %u bytes)", sizes[i],
                entry.readSucceeded ? "ok" : "FAILED", static_cast<unsigned long>(entry.readMs),
                static_cast<unsigned>(totalRead));
    if (!entry.readSucceeded) {
      entry.errorDetail = "read failed or size mismatch";
      SD.remove(path);
      continue;
    }

    entry.verifySucceeded = memcmp(writeDigest, readDigest, 32) == 0;
    Log::printf("[sdtest] fileio %u bytes: byte-for-byte verify -> %s", sizes[i],
                entry.verifySucceeded ? "ok" : "FAILED");
    if (!entry.verifySucceeded) {
      entry.errorDetail = "content mismatch (network/write path fine, storage corrupted bytes)";
    }

    SD.remove(path);
  }
  SD.rmdir(kIoTestDir);
}

}  // namespace

Results run() {
  Results results;
  results.format = runFormatTest();
  if (results.format.mountedBefore) {
    results.cardTotalBytes = SD.cardSize();
    results.cardUsedBytes = SD.usedBytes();
  }
  runFileIoTest(results);
  return results;
}

}  // namespace SdTest
