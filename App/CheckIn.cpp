#include "CheckIn.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <time.h>

#include "Config.h"
#include "Identity.h"
#include "Log.h"
#include "Tls.h"

namespace CheckIn {
namespace {

constexpr const char* kPath = "/api/checkin";

String nowAsIso8601Utc() {
  time_t now = time(nullptr);
  struct tm utc;
  gmtime_r(&now, &utc);
  char buffer[21];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return String(buffer);
}

}  // namespace

Result perform() {
  Result result;

  NetworkClientSecure client;
  if (!Tls::configure(client)) {
    Log::line("[checkin] TLS setup failed, skipping this check-in");
    return result;
  }

  HTTPClient http;
  const String url = String("https://") + Config::kServiceHost + kPath;
  if (!http.begin(client, url)) {
    Log::line("[checkin] could not begin request, skipping this check-in");
    return result;
  }
  http.setTimeout(Config::kHttpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());
  http.addHeader("Content-Type", "application/json");

  JsonDocument requestDoc;
  requestDoc["deviceUtcTimestamp"] = nowAsIso8601Utc();
  requestDoc["firmwareVersion"] = Identity::installedAppVersion();
  // No battery on this board - see CheckIn.h's own remarks.
  requestDoc["batteryPercent"] = 100;
  requestDoc["charging"] = true;

  String body;
  serializeJson(requestDoc, body);

  const int status = http.POST(body);
  if (status == 401) {
    result.secretRejected = true;
    http.end();
    Log::line("[checkin] rejected: device secret no longer valid (401)");
    return result;
  }
  if (status != 200) {
    http.end();
    Log::printf("[checkin] failed, http status=%d", status);
    return result;
  }

  JsonDocument responseDoc;
  const DeserializationError err = deserializeJson(responseDoc, http.getStream());
  http.end();
  if (err) {
    Log::line("[checkin] response was not valid JSON");
    return result;
  }

  result.ok = true;
  result.acknowledged = responseDoc["acknowledged"] | false;
  result.updateAvailable = responseDoc["updateAvailable"] | false;
  result.debugStreamRequested = responseDoc["debugStreamRequested"] | false;
  result.utcOffsetMinutes = responseDoc["utcOffsetMinutes"] | 0;
  result.isDaytime = responseDoc["isDaytime"] | true;
  const int intervalSeconds = responseDoc["checkInIntervalSeconds"] | 300;
  result.intervalMs = static_cast<uint32_t>(intervalSeconds) * 1000UL;
  Log::printf(
      "[checkin] ok (acknowledged=%d updateAvailable=%d debugStream=%d intervalSeconds=%d "
      "utcOffsetMinutes=%d isDaytime=%d)",
      result.acknowledged, result.updateAvailable, result.debugStreamRequested, intervalSeconds,
      result.utcOffsetMinutes, result.isDaytime);
  return result;
}

}  // namespace CheckIn
