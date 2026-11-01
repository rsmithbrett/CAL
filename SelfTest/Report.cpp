#include "Report.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>

#include "Clock.h"
#include "Config.h"
#include "Identity.h"
#include "Log.h"
#include "Tls.h"

namespace Report {
namespace {

/// Sets a member only when there is something to say. The contract reads an
/// absent optional member as "this run did not measure it", which is a
/// different finding from a zero or an empty string.
void setIfPresent(JsonObject object, const char* key, const String& value) {
  if (value.length() > 0) {
    object[key] = value;
  }
}

void buildScreen(JsonObject screen, const ScreenTest::Result& result, uint32_t durationMs) {
  screen["passed"] = result.passed;
  screen["durationMs"] = durationMs;
  setIfPresent(screen, "detail", result.detail);
}

void buildSdCard(JsonObject sdCard, const SdTest::Results& sd, uint32_t durationMs) {
  // A card that never mounted means the round trip was never attempted, which
  // is why the mount is part of the verdict rather than folded into it.
  sdCard["passed"] = sd.format.mountedBefore && sd.format.formatSucceeded;
  sdCard["reformatAttempted"] = sd.format.attempted;
  sdCard["reformatSucceeded"] = sd.format.formatSucceeded;

  // One capacity reading, taken at mount. capacityBytesAfterReformat stays
  // absent because this sketch does not re-measure after the round trip, and
  // repeating the first figure there would assert a comparison nobody made.
  if (sd.format.mountedBefore && sd.cardTotalBytes > 0) {
    sdCard["capacityBytesBeforeReformat"] = sd.cardTotalBytes;
  }

  sdCard["durationMs"] = durationMs;

  String detail = sd.format.errorDetail;
  if (sd.format.mountedBefore && sd.cardUsedBytes > 0) {
    if (detail.length() > 0) {
      detail += "; ";
    }
    detail += "used ";
    detail += String(static_cast<uint32_t>(sd.cardUsedBytes / 1024));
    detail += "KB of ";
    detail += String(static_cast<uint32_t>(sd.cardTotalBytes / 1024));
    detail += "KB at mount";
  }
  setIfPresent(sdCard, "detail", detail);
}

void buildFileIo(JsonObject fileIo, const SdTest::Results& sd, uint32_t durationMs) {
  uint64_t bytesWritten = 0;
  uint64_t bytesRead = 0;
  bool everyRunPassed = sd.fileIoCount > 0;
  bool everyRunVerified = sd.fileIoCount > 0;
  uint32_t measuredMs = 0;
  String detail;

  for (uint8_t i = 0; i < sd.fileIoCount; ++i) {
    const SdTest::FileIoResult& entry = sd.fileIo[i];
    if (entry.writeSucceeded) {
      bytesWritten += entry.sizeBytes;
    }
    if (entry.readSucceeded) {
      bytesRead += entry.sizeBytes;
    }
    if (!entry.verifySucceeded) {
      everyRunVerified = false;
    }
    if (!(entry.writeSucceeded && entry.readSucceeded && entry.verifySucceeded)) {
      everyRunPassed = false;
    }
    measuredMs += entry.writeMs + entry.readMs;

    // One clause per size, so the per-run figures the structured fields above
    // collapse are still readable by whoever opens the report.
    if (detail.length() > 0) {
      detail += "; ";
    }
    detail += String(entry.sizeBytes / 1024);
    detail += "KB w=";
    detail += entry.writeSucceeded ? String(entry.writeMs) + "ms" : String("fail");
    detail += " r=";
    detail += entry.readSucceeded ? String(entry.readMs) + "ms" : String("fail");
    detail += entry.verifySucceeded ? " verified" : " MISMATCH";
    if (entry.errorDetail.length() > 0) {
      detail += " (";
      detail += entry.errorDetail;
      detail += ")";
    }
  }

  fileIo["passed"] = everyRunPassed;
  fileIo["bytesWritten"] = bytesWritten;
  fileIo["bytesRead"] = bytesRead;
  fileIo["verifiedOk"] = everyRunVerified;
  // The sum of the per-run write and read times where those were measured,
  // falling back to the caller's wall clock for the category when no run
  // reported one.
  fileIo["durationMs"] = measuredMs > 0 ? measuredMs : durationMs;
  setIfPresent(fileIo, "detail", detail);
}

void buildMemory(JsonObject memory, const MemoryTest::Results& results, uint32_t durationMs) {
  bool everyAllocationSucceeded = results.count > 0;
  uint32_t peakAllocatedBytes = 0;
  String detail;

  for (uint8_t i = 0; i < results.count; ++i) {
    const MemoryTest::AllocResult& entry = results.allocations[i];
    if (!entry.allocationSucceeded) {
      everyAllocationSucceeded = false;
    } else if (entry.requestedBytes > peakAllocatedBytes) {
      peakAllocatedBytes = entry.requestedBytes;
    }

    if (detail.length() > 0) {
      detail += "; ";
    }
    detail += String(entry.requestedBytes / 1024);
    detail += "KB ";
    detail += entry.allocationSucceeded ? "ok" : "FAILED";
    detail += " block ";
    detail += String(entry.maxAllocHeapBefore);
    detail += "->";
    detail += String(entry.maxAllocHeapAfter);
  }

  memory["passed"] = everyAllocationSucceeded;

  // The span across the whole category: the first attempt's before-figures and
  // the last attempt's after-figures. Every attempt is freed immediately after
  // its own snapshot (MemoryTest::run), so these bound the run rather than
  // describing any one allocation.
  if (results.count > 0) {
    const MemoryTest::AllocResult& first = results.allocations[0];
    const MemoryTest::AllocResult& last = results.allocations[results.count - 1];
    memory["freeHeapBeforeBytes"] = first.freeHeapBefore;
    memory["freeHeapAfterBytes"] = last.freeHeapAfter;
    memory["largestFreeBlockBeforeBytes"] = first.maxAllocHeapBefore;
    memory["largestFreeBlockAfterBytes"] = last.maxAllocHeapAfter;
  }

  memory["peakAllocatedBytes"] = peakAllocatedBytes;
  memory["durationMs"] = durationMs;
  setIfPresent(memory, "detail", detail);
}

/// The run's own tally, as free text. The server computes its own verdict from
/// the categories rather than reading a count, so this is for a person reading
/// the stored report and nothing parses it.
String buildNotes(const ScreenTest::Result& screen, const SdTest::Results& sd,
                  const MemoryTest::Results& memory) {
  uint16_t totalRun = 0;
  uint16_t totalPassed = 0;

  ++totalRun;
  if (screen.passed) ++totalPassed;

  ++totalRun;
  // A failed mount is a failed test, not a skip - see SdTest.h's own remarks.
  if (sd.format.formatSucceeded && sd.format.mountedBefore) ++totalPassed;

  for (uint8_t i = 0; i < sd.fileIoCount; ++i) {
    ++totalRun;
    const SdTest::FileIoResult& entry = sd.fileIo[i];
    if (entry.writeSucceeded && entry.readSucceeded && entry.verifySucceeded) ++totalPassed;
  }

  for (uint8_t i = 0; i < memory.count; ++i) {
    ++totalRun;
    if (memory.allocations[i].allocationSucceeded) ++totalPassed;
  }

  String notes = String(totalRun) + " checks, " + String(totalPassed) + " passed, " +
                 String(totalRun - totalPassed) + " failed";
  if (!Clock::isSet()) {
    notes += "; clock not synced, timestamps omitted";
  }
  return notes;
}

void buildDoc(JsonDocument& doc, const ScreenTest::Result& screen, const SdTest::Results& sd,
              const MemoryTest::Results& memory, const Timing& timing) {
  doc["firmwareVersion"] = Config::kSelfTestVersion;

  // Omitted rather than sent as the epoch when the clock never synced. An
  // absent DateTimeOffset binds to its own default on the server, so a zero
  // here would be stored as an instant in year one instead of as nothing.
  const String startedAt = Clock::toIso8601(timing.startedAtUtc);
  const String completedAt = Clock::toIso8601(timing.completedAtUtc);
  if (startedAt.length() > 0) {
    doc["startedAtUtc"] = startedAt;
  }
  if (completedAt.length() > 0) {
    doc["completedAtUtc"] = completedAt;
  }

  buildScreen(doc["screen"].to<JsonObject>(), screen, timing.screenMs);
  buildSdCard(doc["sdCard"].to<JsonObject>(), sd, timing.sdFormatMs);
  buildFileIo(doc["fileIo"].to<JsonObject>(), sd, timing.fileIoMs);
  buildMemory(doc["memory"].to<JsonObject>(), memory, timing.memoryMs);

  doc["notes"] = buildNotes(screen, sd, memory);
}

/// Best-effort POST. Returns false for anything short of an HTTP 200 - TLS
/// setup failure, no route to the server, a refused body - and logs which,
/// but never throws or halts the caller.
bool postReport(const String& body) {
  NetworkClientSecure client;
  if (!Tls::configure(client)) {
    Log::line("[selftest-report] TLS setup failed - report was NOT sent to the server "
              "(full JSON above still reached the debug stream)");
    return false;
  }

  HTTPClient http;
  const String url = String("https://") + Config::kServiceHost + Config::kSelfTestReportPath;
  if (!http.begin(client, url)) {
    Log::line("[selftest-report] could not begin HTTP request - report was NOT sent");
    return false;
  }
  http.setTimeout(Config::kHttpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());
  http.addHeader("Content-Type", "application/json");

  const int status = http.POST(body);
  http.end();

  if (status != 200) {
    // A negative status is HTTPClient's own error code rather than a server
    // answer - see HTTPClient.h's HTTPC_ERROR_* constants. A chain that cannot
    // be validated arrives here, which is what an unsynced clock produces.
    Log::printf(
        "[selftest-report] POST %s returned status=%d - report was NOT accepted (full JSON is "
        "above in the debug stream regardless)",
        Config::kSelfTestReportPath, status);
    return false;
  }

  Log::line("[selftest-report] POST accepted (200) by the server");
  return true;
}

}  // namespace

bool sendAndLog(const ScreenTest::Result& screen, const SdTest::Results& sd,
                const MemoryTest::Results& memory, const Timing& timing) {
  JsonDocument doc;
  buildDoc(doc, screen, sd, memory, timing);

  String body;
  serializeJson(doc, body);

  // Logged in full, unconditionally, before the POST is even attempted - the
  // debug stream carries the results whether or not the ingestion endpoint
  // accepts them.
  Log::line("[selftest-report] full report JSON follows:");
  Log::line(body);

  return postReport(body);
}

}  // namespace Report
