#include "Aircraft.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>

#include "Cards.h"
#include "Config.h"
#include "Display.h"
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

  result.radiusMiles = doc["radiusMiles"] | 10.0;

  JsonArrayConst aircraft = doc["aircraft"].as<JsonArrayConst>();
  if (aircraft.isNull() || aircraft.size() == 0) {
    result.status = Status::Empty;
    char buffer[48];
    snprintf(buffer, sizeof(buffer), "No aircraft within %.0f mi right now.", result.radiusMiles);
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

// ---------------------------------------------------------------------------
// The card descriptor. See the equivalent block at the bottom of Weather.cpp
// for why registration happens here rather than in App.ino.
//
// Registered as a *list* card even though the server currently gives us
// exactly one sighting - the nearest. That is the honest description of the
// card's shape: MyAircraftService returns a distance-sorted list and this
// module takes element 0 only because nothing had a use for the rest. A
// later server change that hands over the whole list needs cardItemCount()
// and cardDraw() to start reading an index, and nothing in the scheduler to
// change at all. Registering it as an interstitial today would have to be
// undone then.
// ---------------------------------------------------------------------------
namespace {

Result gLast;
bool gEverFetched = false;

void cardFetch() {
  gLast = fetchMine();
  gEverFetched = true;
  if (gLast.status == Status::Ok) {
    Log::printf("[aircraft] card updated: %s alt=%dft speed=%.0fkts heading=%.0f dist=%.1fmi",
                gLast.nearest.callsign.c_str(), gLast.nearest.altitudeFeet,
                gLast.nearest.speedKnots, gLast.nearest.headingDegrees,
                gLast.nearest.distanceMiles);
  }
}

/// Same reasoning as Weather's: one item once anything has been fetched.
/// Empty ("nothing overhead right now") counts as an item rather than zero -
/// it is a real, informative message this card has always shown, and the
/// scheduler's empty-card skipping is for cards with nothing at all to say.
uint16_t cardItemCount() { return gEverFetched ? 1 : 0; }

/// A plane nearly directly overhead earns the longer dwell. Threshold scales
/// with the configured radius rather than being a fixed mile count, so it
/// still means "practically overhead" whether the owner tracks 3 miles or
/// 30 - the same 20%-of-radius rule CYD-Dickey uses, with the same 1-mile
/// floor so a very small radius does not make every sighting notable.
bool cardIsNotable(uint16_t) {
  if (gLast.status != Status::Ok) {
    return false;
  }
  const double threshold = max(1.0, gLast.radiusMiles * 0.2);
  return gLast.nearest.distanceMiles <= threshold;
}

void cardDraw(uint16_t) {
  if (gLast.status == Status::Ok) {
    Display::showAircraftCard(gLast.nearest.callsign, gLast.nearest.altitudeFeet,
                              gLast.nearest.speedKnots, gLast.nearest.headingDegrees,
                              gLast.nearest.distanceMiles, "Updated just now");
    return;
  }

  // Empty (fetch worked, nothing in range right now) and the two resting
  // states read as ordinary/muted; auth and network trouble read amber -
  // same isProblem split Weather's card makes, just with a third muted case
  // this card has and weather doesn't.
  const bool isRestingState = gLast.status == Status::NotActivated ||
                              gLast.status == Status::ProviderDisabled ||
                              gLast.status == Status::Empty;
  const String headline = gLast.status == Status::Empty ? "Nothing overhead right now"
                          : isRestingState              ? "Aircraft overhead is not showing yet"
                                                        : "Could not load aircraft data";
  Display::showAircraftStatus(headline, gLast.message, /*isProblem=*/!isRestingState);
}

[[maybe_unused]] const bool kRegistered = [] {
  Cards::CardSpec spec;
  spec.id = "aircraft";
  spec.kind = Cards::Kind::List;
  spec.fetch = cardFetch;
  spec.itemCount = cardItemCount;
  spec.draw = cardDraw;
  spec.isNotable = cardIsNotable;
  spec.order = 2;
  spec.dwellSeconds = 8;
  spec.notableDwellSeconds = 20;
  return Cards::registerCard(spec);
}();

}  // namespace

}  // namespace Aircraft
