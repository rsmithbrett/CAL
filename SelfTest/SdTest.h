#pragma once

#include <Arduino.h>

/// Categories 2 and 3 of the four SelfTest suites: a real mount/format/
/// reformat round-trip against the actual SD hardware, and a file I/O
/// stress test across a representative range of asset sizes.
///
/// Deliberately NOT App/Assets.cpp's wipeCache(): that function silently
/// reports "0 files removed" success even when the card was never mounted
/// at all, which is exactly the failure mode that cost hours of log
/// correlation tonight (see the incident this whole sketch exists to
/// prevent a repeat of). Every step here reports its own honest pass/fail,
/// starting with the mount itself, before anything downstream is allowed to
/// paper over it.
namespace SdTest {

/// Category 2: mount, then a create/write/read/verify/delete round trip
/// against a handful of files, standing in for what a real reformat
/// workflow needs to prove works. Not a literal low-level filesystem
/// format - the Arduino SD library exposes no such call - see run()'s own
/// remarks in SdTest.cpp for why this is the honest thing to call
/// "formatSucceeded" instead.
struct FormatResult {
  bool attempted = true;
  bool mountedBefore = false;
  bool mountedAfter = false;
  bool formatSucceeded = false;
  String errorDetail;
};

/// Category 3: one entry per file size tested.
struct FileIoResult {
  uint32_t sizeBytes = 0;
  bool writeSucceeded = false;
  uint32_t writeMs = 0;
  bool readSucceeded = false;
  uint32_t readMs = 0;
  bool verifySucceeded = false;
  String errorDetail;
};

constexpr uint8_t kMaxFileIoResults = 4;

struct Results {
  FormatResult format;
  FileIoResult fileIo[kMaxFileIoResults];
  uint8_t fileIoCount = 0;
  uint64_t cardTotalBytes = 0;
  uint64_t cardUsedBytes = 0;
};

/// Runs both categories back to back against the same mount - a fresh
/// SD.begin() every call, not App/SdStorage.cpp's cached gReady, because
/// this sketch's whole purpose is finding out whether the mount itself
/// actually works right now, not reusing a possibly-stale answer from
/// earlier in the run.
Results run();

}  // namespace SdTest
