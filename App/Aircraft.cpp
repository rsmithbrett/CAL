#include "Aircraft.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>

#include "Config.h"
#include "Identity.h"
#include "Log.h"
#include "Tls.h"

namespace Aircraft {
namespace {

constexpr const char* kPath = "/api/myaircraft/mine";

// Identical shape and reasoning to Weather.cpp's parseRefusal - the same
// ContentProviderGate produces this 403 body for every device-facing content
// route, aircraft included (see ContentProviderGate.cs).
Result parseRefusal(const String& body) {
  Result result;
  result.status = Status::NetworkError;

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    result.message = "Cannot reach the aircraft service.";
    return result;
  }

  const char* reason = doc["reason"] | "";
  const char* message = doc["message"] | "";
  result.message = strlen(message) > 0 ? String(message) : "This card is unavailable.";

  if (strcmp(reason, "device_not_activated") == 0) {
    result.status = Status::NotActivated;
  } else if (strcmp(reason, "content_provider_disabled") == 0) {
    result.status = Status::ProviderDisabled;
  }
  Log::printf("[aircraft] refused (%s): %s", reason, result.message.c_str());
  return result;
}

}  // namespace

Result fetchMine() {
  Result result;

  NetworkClientSecure client;
  if (!Tls::configure(client)) {
    result.message = "Cannot verify the service's identity.";
    Log::line("[aircraft] TLS setup failed");
    return result;
  }

  HTTPClient http;
  const String url = String("https://") + Config::kServiceHost + kPath;
  if (!http.begin(client, url)) {
    result.message = "Cannot reach the aircraft service.";
    Log::line("[aircraft] could not begin request");
    return result;
  }
  http.setTimeout(Config::kHttpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());

  const int status = http.GET();

  if (status == 401) {
    http.end();
    result.status = Status::AuthError;
    result.message = "Cannot verify this device. Contact support.";
    Log::line("[aircraft] auth rejected (401)");
    return result;
  }

  if (status == 403) {
    const String body = http.getString();
    http.end();
    return parseRefusal(body);
  }

  if (status != 200) {
    http.end();
    result.message = "Cannot reach the aircraft service.";
    Log::printf("[aircraft] unexpected http status=%d", status);
    return result;
  }

  // Only the fields Display::showAircraftCard() reads are worth keeping in
  // the filter - same rationale as CYD-Dickey's Aircraft.cpp filtering
  // adsb.lol's couple-dozen raw fields down to five, just applied to this
  // server's already-trimmed AircraftResult/AircraftSighting shape instead.
  JsonDocument filter;
  filter["radiusMiles"] = true;
  filter["aircraft"][0]["callsign"] = true;
  filter["aircraft"][0]["altitudeFeet"] = true;
  filter["aircraft"][0]["speedKnots"] = true;
  filter["aircraft"][0]["headingDegrees"] = true;
  filter["aircraft"][0]["distanceMiles"] = true;

  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    result.message = "The aircraft service sent something unreadable.";
    Log::line("[aircraft] response was not valid JSON");
    return result;
  }

  JsonArrayConst aircraft = doc["aircraft"].as<JsonArrayConst>();
  if (aircraft.isNull() || aircraft.size() == 0) {
    result.status = Status::Empty;
    const double radiusMiles = doc["radiusMiles"] | 10.0;
    char buffer[48];
    snprintf(buffer, sizeof(buffer), "No aircraft within %.0f mi right now.", radiusMiles);
    result.message = String(buffer);
    return result;
  }

  JsonVariantConst nearest = aircraft[0];
  result.status = Status::Ok;
  result.nearest.callsign = String((const char*)(nearest["callsign"] | "UNKNOWN"));
  result.nearest.altitudeFeet = nearest["altitudeFeet"] | 0;
  result.nearest.speedKnots = nearest["speedKnots"] | 0.0;
  result.nearest.headingDegrees = nearest["headingDegrees"] | 0.0;
  result.nearest.distanceMiles = nearest["distanceMiles"] | 0.0;
  return result;
}

}  // namespace Aircraft
