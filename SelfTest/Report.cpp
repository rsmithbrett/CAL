#include "Report.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>

#include "Config.h"
#include "Identity.h"
#include "Log.h"
#include "Tls.h"

namespace Report {
namespace {

void buildDoc(JsonDocument& doc, const ScreenTest::Result& screen, const SdTest::Results& sd,
              const MemoryTest::Results& memory) {
  doc["firmwareVersion"] = Config::kSelfTestVersion;

  JsonObject screenObj = doc["screenTest"].to<JsonObject>();
  screenObj["attempted"] = screen.attempted;
  screenObj["passed"] = screen.passed;
  screenObj["detail"] = screen.detail;

  JsonObject sdFormatObj = doc["sdFormat"].to<JsonObject>();
  sdFormatObj["attempted"] = sd.format.attempted;
  sdFormatObj["mountedBefore"] = sd.format.mountedBefore;
  sdFormatObj["mountedAfter"] = sd.format.mountedAfter;
  sdFormatObj["formatSucceeded"] = sd.format.formatSucceeded;
  sdFormatObj["errorDetail"] = sd.format.errorDetail;

  JsonArray fileIoArray = doc["fileIo"].to<JsonArray>();
  for (uint8_t i = 0; i < sd.fileIoCount; ++i) {
    const SdTest::FileIoResult& entry = sd.fileIo[i];
    JsonObject obj = fileIoArray.add<JsonObject>();
    obj["sizeBytes"] = entry.sizeBytes;
    obj["writeSucceeded"] = entry.writeSucceeded;
    obj["writeMs"] = entry.writeMs;
    obj["readSucceeded"] = entry.readSucceeded;
    obj["readMs"] = entry.readMs;
    obj["verifySucceeded"] = entry.verifySucceeded;
    obj["errorDetail"] = entry.errorDetail;
  }

  JsonArray memArray = doc["memoryLoad"].to<JsonArray>();
  for (uint8_t i = 0; i < memory.count; ++i) {
    const MemoryTest::AllocResult& entry = memory.allocations[i];
    JsonObject obj = memArray.add<JsonObject>();
    obj["requestedBytes"] = entry.requestedBytes;
    obj["allocationSucceeded"] = entry.allocationSucceeded;
    obj["freeHeapBefore"] = entry.freeHeapBefore;
    obj["freeHeapAfter"] = entry.freeHeapAfter;
    obj["maxAllocHeapBefore"] = entry.maxAllocHeapBefore;
    obj["maxAllocHeapAfter"] = entry.maxAllocHeapAfter;
  }

  uint16_t totalRun = 0;
  uint16_t totalPassed = 0;

  ++totalRun;
  if (screen.passed) ++totalPassed;

  ++totalRun;
  // A failed mount is a failed test, not a skip - see SdTest.h's own
  // remarks and this task's brief on why wipeCache()'s old silent-success
  // behaviour is exactly what this sketch must not repeat.
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

  JsonObject summary = doc["overallSummary"].to<JsonObject>();
  summary["totalTestsRun"] = totalRun;
  summary["totalPassed"] = totalPassed;
  summary["totalFailed"] = totalRun - totalPassed;
}

/// Best-effort POST. Returns false for anything short of an HTTP 200 -
/// TLS setup failure, no route to the server, the endpoint not existing yet
/// (404), or a live but rejecting endpoint - and logs exactly which, but
/// never throws or halts the caller. Designed against the parallel
/// server-side task's brief rather than a confirmed-live endpoint; a shape
/// or path mismatch is expected to surface here as a non-200 status, not a
/// crash, and gets reconciled once both sides can compare notes.
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
    Log::printf(
        "[selftest-report] POST %s returned status=%d - report was NOT accepted (this is "
        "expected if the server-side ingestion endpoint hasn't landed yet; full JSON is above "
        "in the debug stream regardless)",
        Config::kSelfTestReportPath, status);
    return false;
  }

  Log::line("[selftest-report] POST accepted (200) by the server");
  return true;
}

}  // namespace

bool sendAndLog(const ScreenTest::Result& screen, const SdTest::Results& sd,
                const MemoryTest::Results& memory) {
  JsonDocument doc;
  buildDoc(doc, screen, sd, memory);

  String body;
  serializeJson(doc, body);

  // Logged in full, unconditionally, before the POST is even attempted -
  // see Report.h's own remarks: this is what makes results visible via the
  // debug stream immediately even if the ingestion endpoint isn't live yet
  // or the contract doesn't match perfectly.
  Log::line("[selftest-report] full report JSON follows:");
  Log::line(body);

  return postReport(body);
}

}  // namespace Report
