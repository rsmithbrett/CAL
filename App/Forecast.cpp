#include "Forecast.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>

#include "Cards.h"
#include "Config.h"
#include "Display.h"
#include "Identity.h"
#include "Log.h"
#include "Tls.h"

namespace Forecast {
namespace {

constexpr const char* kCardId = "forecast";

// Two fixed paths rather than building the query string at request time -
// the value this card ever sends is exactly one of these two literals (see
// wantsTarget() below), so there is nothing to build.
constexpr const char* kPathHome = "/api/myweather/forecast?location=home";
constexpr const char* kPathTarget = "/api/myweather/forecast?location=target";

// Identical shape and reasoning to Weather.cpp's/Listings.cpp's own
// parseRefusal - the same ContentProviderGate produces this 403 body for
// every device-facing content route, forecast included.
Result parseRefusal(const String& body) {
  Result result;
  result.status = Status::NetworkError;

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    result.message = "Cannot reach the forecast service.";
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
  Log::printf("[forecast] refused (%s): %s", reason, result.message.c_str());
  return result;
}

// Same city-over-postal-code preference as Weather.cpp's own
// describeLocation(), applied to this endpoint's own top-level
// city/state/postalCode instead of a nested home/target object.
String describeLocation(JsonVariantConst body) {
  const char* city = body["city"] | "";
  const char* state = body["state"] | "";
  if (strlen(city) > 0 && strlen(state) > 0) {
    return String(city) + ", " + String(state);
  }
  if (strlen(city) > 0) {
    return String(city);
  }
  const char* postalCode = body["postalCode"] | "";
  return String(postalCode);
}

}  // namespace

Result fetch(bool useTarget) {
  Result result;

  NetworkClientSecure client;
  if (!Tls::configure(client)) {
    result.message = "Cannot verify the service's identity.";
    Log::line("[forecast] TLS setup failed");
    return result;
  }

  HTTPClient http;
  const String url =
      String("https://") + Config::kServiceHost + (useTarget ? kPathTarget : kPathHome);
  if (!http.begin(client, url)) {
    result.message = "Cannot reach the forecast service.";
    Log::line("[forecast] could not begin request");
    return result;
  }
  http.setTimeout(Config::kHttpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());

  const int status = http.GET();

  if (status == 401) {
    http.end();
    result.status = Status::AuthError;
    result.message = "Cannot verify this device. Contact support.";
    Log::line("[forecast] auth rejected (401)");
    return result;
  }

  if (status == 403) {
    const String body = http.getString();
    http.end();
    return parseRefusal(body);
  }

  if (status != 200) {
    http.end();
    result.message = "Cannot reach the forecast service.";
    Log::printf("[forecast] unexpected http status=%d", status);
    return result;
  }

  // Only the fields this card actually draws survive the filter - same
  // heap-conservation reasoning as Listings.cpp's own filter. The endpoint's
  // own trim already keeps the untrimmed size down to ~1,500 bytes worst
  // case (see MyWeatherEndpoints.MaxForecastPeriods's own remarks on the
  // incident that number exists to prevent), and it deliberately never
  // returns both Home and Target in one response - but a filter here still
  // costs nothing and means a future field added to this endpoint never grows
  // what this card itself has to hold in memory to parse. A single index
  // anywhere inside "periods" (ArduinoJson's own filter semantics) keeps
  // those fields for every element the array actually has, not just index 0.
  JsonDocument filter;
  filter["city"] = true;
  filter["state"] = true;
  filter["postalCode"] = true;
  filter["periods"][0]["name"] = true;
  filter["periods"][0]["isDaytime"] = true;
  filter["periods"][0]["temperature"] = true;
  filter["periods"][0]["temperatureUnit"] = true;
  filter["periods"][0]["shortForecast"] = true;

  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    result.message = "The forecast service sent something unreadable.";
    Log::line("[forecast] response was not valid JSON");
    return result;
  }

  result.location = describeLocation(doc.as<JsonVariantConst>());

  JsonArrayConst periods = doc["periods"].as<JsonArrayConst>();
  if (periods.isNull() || periods.size() == 0) {
    // Always a 200 with an object, never a bare error - a location that
    // could not be resolved at all reports itself the same way an ordinary
    // "nothing to show yet" response does (see
    // MyWeatherEndpoints.ForDeviceForecast's own remarks: city/state/
    // postalCode/periods all empty rather than a top-level JSON null). This
    // card reports nothing to draw either way, so there is no separate
    // "location unresolved" message worth inventing here.
    result.status = Status::Empty;
    result.message = "No forecast is available yet.";
    return result;
  }

  result.status = Status::Ok;
  result.count = 0;
  for (JsonVariantConst period : periods) {
    if (result.count >= kMaxPeriods) {
      break;
    }
    PeriodInfo& info = result.periods[result.count];
    info.name = String((const char*)(period["name"] | ""));
    info.isDaytime = period["isDaytime"] | true;
    info.temperature = period["temperature"] | 0;
    info.unit = String((const char*)(period["temperatureUnit"] | "F"));
    info.shortForecast = String((const char*)(period["shortForecast"] | ""));
    result.count++;
  }

  return result;
}

// ---------------------------------------------------------------------------
// The card descriptor. See the equivalent block at the bottom of
// Weather.cpp/Listings.cpp for why registration happens here rather than in
// App.ino.
// ---------------------------------------------------------------------------
namespace {

/// This card's own Location choice, read straight off its descriptor rather
/// than cached in a global - same reasoning Announcement.cpp's currentText()
/// gives for its own field: the policy can change under this module at any
/// check-in, and a cached choice would keep querying the location an admin
/// just switched away from until the next fetch happened to re-read it
/// anyway, which re-reading directly makes moot. "target" (case-insensitive)
/// is the only value that means Target; absent, blank, "home", or a typo all
/// mean Home, mirroring MyWeatherEndpoints' own tolerant default on the
/// server exactly (see that endpoint's own remarks on why an unrecognised
/// value is never rejected outright).
bool wantsTarget() {
  const int8_t index = Cards::indexOf(kCardId);
  if (index < 0) {
    return false;
  }
  return strcasecmp(Cards::at(static_cast<uint8_t>(index)).location, "target") == 0;
}

Result gLast;
bool gEverFetched = false;

/// millis() when gLast last became an Ok result - same field, same reasoning
/// as Weather.cpp's/Listings.cpp's own gLastOkMs.
unsigned long gLastOkMs = 0;

/// Unsigned subtraction, correct across the millis() rollover at ~49 days -
/// identical to Weather.cpp's/Listings.cpp's own describeFreshness(),
/// duplicated rather than shared for the same reason Listings.cpp's own copy
/// is: the three cards' Result types are unrelated and a shared helper would
/// need a fourth file just to hold one function used three times.
String describeFreshness(unsigned long fetchedAtMs) {
  const unsigned long ageMinutes = (millis() - fetchedAtMs) / 60000UL;
  if (ageMinutes == 0) {
    return "Updated just now";
  }
  if (ageMinutes == 1) {
    return "Updated 1 min ago";
  }
  return String("Updated ") + ageMinutes + " min ago";
}

void cardFetch() {
  const bool useTarget = wantsTarget();
  gLast = fetch(useTarget);
  gEverFetched = true;
  if (gLast.status == Status::Ok) {
    gLastOkMs = millis();
    Log::printf("[forecast] card updated (%s): %u period(s), starting with '%s' %d%s %s",
                useTarget ? "target" : "home", gLast.count, gLast.periods[0].name.c_str(),
                gLast.periods[0].temperature, gLast.periods[0].unit.c_str(),
                gLast.periods[0].shortForecast.c_str());
  }
}

/// Real count while Ok (capped at kMaxPeriods by fetch() itself), one item
/// for any other status - same "a message is content too" tolerance
/// Weather's/Listings' own cardItemCount() already give their resting and
/// error states, and zero before the first fetch so the scheduler passes
/// over this card entirely until it has an answer at all.
uint16_t cardItemCount() {
  if (!gEverFetched) {
    return 0;
  }
  return gLast.status == Status::Ok ? gLast.count : 1;
}

void cardDraw(uint16_t itemIndex) {
  if (gLast.status == Status::Ok) {
    if (itemIndex >= gLast.count) {
      itemIndex = 0;
    }
    const PeriodInfo& period = gLast.periods[itemIndex];
    Display::showForecastCard(gLast.location, period.name, period.isDaytime, period.temperature,
                              period.unit, period.shortForecast, /*index=*/itemIndex,
                              /*total=*/gLast.count, describeFreshness(gLastOkMs));
    return;
  }

  // NotActivated, ProviderDisabled and Empty are resting states - nothing
  // wrong with the device, just nothing configured or nothing resolved yet -
  // shown muted rather than amber, the same isProblem split Weather's and
  // Listings' own cards make.
  const bool isRestingState = gLast.status == Status::NotActivated ||
                              gLast.status == Status::ProviderDisabled ||
                              gLast.status == Status::Empty;
  const String headline = gLast.status == Status::Empty ? "No forecast available right now"
                          : isRestingState               ? "Forecast is not showing yet"
                                                          : "Could not load the forecast";
  Display::showForecastStatus(headline, gLast.message, /*isProblem=*/!isRestingState);
}

/// Registers this card at static-init time, so App.ino never names it. The
/// registry it writes into is constant-initialised (see the top of
/// CardManager.cpp), so this cannot run before the registry exists.
///
/// List, not Interstitial: like Listings, the server can hand back several
/// periods (up to kMaxPeriods) and this card cycles through them one per
/// dwell rather than showing a single featured reading. Order 4 slots it
/// right after Listings (3) among list cards; the scheduler's own tie-break
/// (Cards.h's `earlier()`) means this ordering only matters relative to other
/// List-kind cards, never against the Interstitials sharing the same numbers.
[[maybe_unused]] const bool kRegistered = [] {
  Cards::CardSpec spec;
  spec.id = kCardId;
  spec.kind = Cards::Kind::List;
  spec.fetch = cardFetch;
  spec.itemCount = cardItemCount;
  spec.draw = cardDraw;
  spec.order = 4;
  spec.dwellSeconds = 10;
  return Cards::registerCard(spec);
}();

}  // namespace

}  // namespace Forecast
