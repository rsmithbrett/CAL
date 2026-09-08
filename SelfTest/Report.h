#pragma once

#include "MemoryTest.h"
#include "ScreenTest.h"
#include "SdTest.h"

/// Assembles the JSON report shape and delivers it two ways: always to the
/// remote debug stream (via Log::line, in full, regardless of what happens
/// next), and best-effort to the server's ingestion endpoint.
///
/// The endpoint (POST /api/selftest/report) is being built concurrently by
/// a parallel session on the server side and may not exist yet, or may not
/// match this shape exactly, at the moment any given SelfTest run happens.
/// Neither case may block or crash this sketch - see sendAndLog()'s own
/// remarks in Report.cpp for how that is enforced.
namespace Report {

/// Builds the report, logs it in full, and attempts the POST. Returns
/// whether the POST itself was accepted (HTTP 200) - purely informational
/// for the caller's own on-screen summary; nothing about the run's actual
/// pass/fail results depends on this.
bool sendAndLog(const ScreenTest::Result& screen, const SdTest::Results& sd,
                const MemoryTest::Results& memory);

}  // namespace Report
