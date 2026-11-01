#pragma once

#include <Arduino.h>

#include "MemoryTest.h"
#include "ScreenTest.h"
#include "SdTest.h"

/// Assembles the JSON report and delivers it two ways: always to the remote
/// debug stream (via Log::line, in full, before the POST is attempted), and
/// best-effort to the server's ingestion endpoint.
///
/// The document's shape is SelfTestReportRequest in the server tree, and the
/// path is Config::kSelfTestReportPath. Both are held against that tree by
/// SelfTestReportWireAgreementTests, which reads this sketch's own files off
/// disk: the top-level member names from Report.cpp and the path from Config.h.
/// A member this sketch sends that the contract does not name is a measurement
/// the server drops, so that test treats one as a failure rather than as
/// forward tolerance.
///
/// The contract takes one object per category where this sketch measures
/// several runs of two of them - file I/O at four sizes, memory at six. Both
/// collapse to one object carrying the totals, plus a per-run breakdown in that
/// object's detail string. The breakdown is free text the server stores and
/// never parses, so a reader gets the per-size figures and a query does not.
namespace Report {

/// How long each category took, measured by the caller around its own run, and
/// the two instants that bound the whole session. The contract carries a
/// durationMs per category and this sketch's test functions do not time
/// themselves, so the caller is the only place these can come from.
struct Timing {
  time_t startedAtUtc = 0;
  time_t completedAtUtc = 0;
  uint32_t screenMs = 0;
  uint32_t sdFormatMs = 0;
  uint32_t fileIoMs = 0;
  uint32_t memoryMs = 0;
};

/// Builds the report, logs it in full, and attempts the POST. Returns whether
/// the POST was accepted (HTTP 200) - informational for the caller's own
/// on-screen summary; nothing about the run's pass/fail results depends on it.
bool sendAndLog(const ScreenTest::Result& screen, const SdTest::Results& sd,
                const MemoryTest::Results& memory, const Timing& timing);

}  // namespace Report
