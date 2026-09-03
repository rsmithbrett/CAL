#include "Weather.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>

#include "Cards.h"
#include "Config.h"
#include "Display.h"
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

// ---------------------------------------------------------------------------
// The card descriptor.
//
// Weather used to be a card only in the sense that App.ino held a
// `CardKind::Weather` enum case, a refreshWeatherCard() that both fetched and
// drew in one breath, and a two-way toggle deciding when to call it. All
// three of those are gone: this module now describes itself to the scheduler
// and the scheduler does not know the word "weather" anywhere.
//
// Note the split between cardFetch() and cardDraw(). The Result lives in
// gLast between them, so drawing is a pure redraw of retained state - which
// is what makes stepping backwards through the card history show the card
// that was actually there rather than whatever a fresh fetch would return
// now.
// ---------------------------------------------------------------------------
namespace {

Result gLast;
bool gEverFetched = false;

void cardFetch() {
  gLast = fetchMine();
  gEverFetched = true;
  if (gLast.status == Status::Ok) {
    Log::printf("[weather] card updated: %s, %d%s, %s", gLast.forecast.location.c_str(),
                gLast.forecast.temperature, gLast.forecast.unit.c_str(),
                gLast.forecast.shortForecast.c_str());
  }
}

/// One item once anything has been fetched, zero before that. A non-Ok
/// status still counts as one item, not zero: "weather is not showing yet"
/// or "could not load weather" is a message worth putting on screen, and the
/// scheduler's empty-card skipping is meant for cards with genuinely nothing
/// to say - not for cards with bad news.
uint16_t cardItemCount() { return gEverFetched ? 1 : 0; }

void cardDraw(uint16_t) {
  if (gLast.status == Status::Ok) {
    Display::showWeatherCard(gLast.forecast.location, gLast.forecast.temperature,
                             gLast.forecast.unit, gLast.forecast.shortForecast,
                             "Updated just now");
    return;
  }

  // NotActivated and ProviderDisabled are resting states an owner or their
  // agent can change server-side at any time - shown muted, not amber, since
  // there is nothing wrong with the device itself. Routed through
  // showWeatherStatus() rather than the black boot-ladder showStatus()/
  // showFailure() - see Display.h's remarks on why content-card problems
  // stay in the white/bannered card family instead.
  const bool isRestingState =
      gLast.status == Status::NotActivated || gLast.status == Status::ProviderDisabled;
  Display::showWeatherStatus(
      isRestingState ? "Weather is not showing yet" : "Could not load weather", gLast.message,
      /*isProblem=*/!isRestingState);
}

/// Registers this card at static-init time, so App.ino never names it. The
/// registry it writes into is constant-initialised (see the top of
/// CardManager.cpp), so this cannot run before the registry exists.
///
/// The values below are built-in defaults only - they hold until the first
/// cardPolicy arrives on a check-in and replaces them, and they match the
/// example policy in the contract this was built against. Weather is an
/// interstitial: it interleaves after every N other cards rather than taking
/// a fixed rotation slot, so a device tracking a long list does not see its
/// weather proportionally less often.
[[maybe_unused]] const bool kRegistered = [] {
  Cards::CardSpec spec;
  spec.id = "weather";
  spec.kind = Cards::Kind::Interstitial;
  spec.fetch = cardFetch;
  spec.itemCount = cardItemCount;
  spec.draw = cardDraw;
  spec.order = 1;
  spec.dwellSeconds = 15;
  spec.interleaveEvery = 5;
  return Cards::registerCard(spec);
}();

}  // namespace

}  // namespace Weather
