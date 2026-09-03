#include "Weather.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>

#include "Config.h"
#include "Identity.h"
#include "Log.h"
#include "Tls.h"

namespace Weather {
namespace {

constexpr const char* kPath = "/api/myweather/mine";

/// Picks Target over Home when the owner has one set - the more useful of the
/// two for a device displayed away from the owner's own address - matching
/// UserWeatherResult's own doc comment on which field callers should prefer.
JsonVariantConst preferredForecast(JsonVariantConst body) {
  JsonVariantConst target = body["target"];
  if (!target.isNull()) {
    return target;
  }
  return body["home"];
}

String describeLocation(JsonVariantConst weatherResult) {
  const char* city = weatherResult["city"] | "";
  const char* state = weatherResult["state"] | "";
  if (strlen(city) > 0 && strlen(state) > 0) {
    return String(city) + ", " + String(state);
  }
  const char* postalCode = weatherResult["postalCode"] | "";
  return String(postalCode);
}

Result parseRefusal(int statusCode, const String& body) {
  Result result;
  result.status = Status::NetworkError;

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    result.message = "Cannot reach the weather service.";
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
  Log::printf("[weather] refused (%s): %s", reason, result.message.c_str());
  return result;
}

}  // namespace

Result fetchMine() {
  Result result;

  NetworkClientSecure client;
  if (!Tls::configure(client)) {
    result.message = "Cannot verify the service's identity.";
    Log::line("[weather] TLS setup failed");
    return result;
  }

  HTTPClient http;
  const String url = String("https://") + Config::kServiceHost + kPath;
  if (!http.begin(client, url)) {
    result.message = "Cannot reach the weather service.";
    Log::line("[weather] could not begin request");
    return result;
  }
  http.setTimeout(Config::kHttpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());

  const int status = http.GET();

  if (status == 401) {
    http.end();
    result.status = Status::AuthError;
    result.message = "Cannot verify this device. Contact support.";
    Log::line("[weather] auth rejected (401)");
    return result;
  }

  if (status == 403) {
    const String body = http.getString();
    http.end();
    return parseRefusal(status, body);
  }

  if (status != 200) {
    http.end();
    result.message = "Cannot reach the weather service.";
    Log::printf("[weather] unexpected http status=%d", status);
    return result;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    result.message = "The weather service sent something unreadable.";
    Log::line("[weather] response was not valid JSON");
    return result;
  }

  JsonVariantConst weatherResult = preferredForecast(doc.as<JsonVariantConst>());
  if (weatherResult.isNull()) {
    result.message = "No home address is set for this device's owner yet.";
    Log::line("[weather] no home/target address on file");
    return result;
  }

  JsonArrayConst periods = weatherResult["periods"].as<JsonArrayConst>();
  if (periods.isNull() || periods.size() == 0) {
    result.message = "No forecast is available yet.";
    Log::line("[weather] no forecast periods in response");
    return result;
  }

  JsonVariantConst current = periods[0];
  result.status = Status::Ok;
  result.forecast.location = describeLocation(weatherResult);
  result.forecast.temperature = current["temperature"] | 0;
  result.forecast.unit = String(current["temperatureUnit"] | "F");
  result.forecast.shortForecast = String(current["shortForecast"] | "");
  return result;
}

}  // namespace Weather
