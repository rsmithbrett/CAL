#include "CheckIn.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <time.h>

#include "Config.h"
#include "Identity.h"
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
    return result;
  }

  HTTPClient http;
  const String url = String("https://") + Config::kServiceHost + kPath;
  if (!http.begin(client, url)) {
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
    return result;
  }
  if (status != 200) {
    http.end();
    return result;
  }

  JsonDocument responseDoc;
  const DeserializationError err = deserializeJson(responseDoc, http.getStream());
  http.end();
  if (err) {
    return result;
  }

  result.ok = true;
  result.acknowledged = responseDoc["acknowledged"] | false;
  result.updateAvailable = responseDoc["updateAvailable"] | false;
  const int intervalSeconds = responseDoc["checkInIntervalSeconds"] | 300;
  result.intervalMs = static_cast<uint32_t>(intervalSeconds) * 1000UL;
  return result;
}

}  // namespace CheckIn
