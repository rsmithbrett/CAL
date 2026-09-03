#include "Telemetry.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>

#include "Config.h"
#include "Identity.h"
#include "Log.h"
#include "Tls.h"

namespace Telemetry {
namespace {

constexpr const char* kPath = "/api/telemetry";

}  // namespace

void report(const char* lastCheckInOutcome) {
  NetworkClientSecure client;
  if (!Tls::configure(client)) {
    Log::line("[telemetry] TLS setup failed, skipping this report");
    return;
  }

  HTTPClient http;
  const String url = String("https://") + Config::kServiceHost + kPath;
  if (!http.begin(client, url)) {
    Log::line("[telemetry] could not begin request, skipping this report");
    return;
  }
  http.setTimeout(Config::kHttpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());
  http.addHeader("Content-Type", "application/json");

  // millis()/1000 wraps back toward 0 alongside millis() itself at ~49.7
  // days of continuous uptime (32-bit overflow), rather than saturating -
  // not worth guarding here. This board reboots on every firmware update
  // already (Loader::requestUpdate()), check-in's own fast path delivers one
  // within a single checkInIntervalMs of it being published, and
  // AppUpdater's independent hourly fallback means an update is never more
  // than about an hour from triggering one even if check-in itself were
  // somehow broken the whole time. A unit actually reaching 49 uninterrupted
  // days without any of that intervening would be a surprise worth its own
  // investigation, not a case this field needs to paper over.
  const uint32_t uptimeSeconds = millis() / 1000UL;
  const int rssi = WiFi.RSSI();
  const uint32_t freeHeap = ESP.getFreeHeap();
  // Cleared by App.ino's setup() the moment WiFi first connects (see
  // Identity.h's own remarks on bootAttempts), so this reads 0 for every
  // device healthy enough to ever reach performCheckIn() at all - expected,
  // not a bug in this reporting path. A device stuck retrying before that
  // point never gets far enough to send telemetry in the first place, and
  // one that never reaches steady state keeps counting until CAL's own
  // anti-brick threshold falls back to the factory partition instead.
  const uint8_t bootCount = Identity::bootAttempts();

  JsonDocument requestDoc;
  requestDoc["uptimeSeconds"] = uptimeSeconds;
  requestDoc["wifiRssiDbm"] = rssi;
  requestDoc["freeHeapBytes"] = freeHeap;
  requestDoc["bootCount"] = bootCount;
  requestDoc["lastCheckInOutcome"] = lastCheckInOutcome;

  String body;
  serializeJson(requestDoc, body);

  const int status = http.POST(body);
  http.end();

  // Any HTTP 200 is success, same as CheckIn's own handling of its response
  // - the body (serverUtcTimestamp/acknowledged) has nothing this firmware
  // acts on, so it is not even parsed here.
  if (status != 200) {
    Log::printf("[telemetry] failed, http status=%d", status);
    return;
  }

  Log::printf("[telemetry] ok (uptimeSeconds=%lu rssi=%d freeHeap=%lu bootCount=%u outcome=%s)",
              static_cast<unsigned long>(uptimeSeconds), rssi,
              static_cast<unsigned long>(freeHeap), bootCount, lastCheckInOutcome);
}

}  // namespace Telemetry
