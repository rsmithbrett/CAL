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

const char* const kCardId = "forecast";
const char* const kCardId2 = "forecast2";
const char* const kCardId3 = "forecast3";
const char* const kCardId4 = "forecast4";
const char* const kCardId5 = "forecast5";

namespace {

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
// The card descriptors - five of them, one per Instance<N> instantiation. See
// Forecast.h's own remarks for the judgment call on why this is the one
// fetch-driven card in this build that got the multi-instance treatment.
// ---------------------------------------------------------------------------
namespace {

/// One forecast card's worth of fetch/itemCount/draw logic, parameterized on
/// `N` to give each instantiation its own registered id AND its own retained
/// fetch state - the same Instance<N> idiom Graphic.cpp established, but
/// with a genuine network fetch (like Graphic's asset resolution) rather
/// than Announcement/QrText's pure descriptor read. Each instantiation gets
/// its own `gLast`/`gEverFetched`/`gLastOkMs` the same way Graphic's own
/// `gCachedId`/`gReady` are per-instantiation: instance 2's forecast going
/// stale has no effect on instance 1 or any other.
template <int N>
struct Instance {
  /// This instance's registered id. Defined only for N = 1 through 5 via the
  /// explicit specializations below - instantiating for any other N is a
  /// link error, the same guardrail Graphic.cpp's own id() has.
  static const char* id();

  static Result gLast;
  static bool gEverFetched;

  /// millis() when gLast last became an Ok result - same field, same
  /// reasoning as Weather.cpp's/Listings.cpp's own gLastOkMs, one copy per
  /// instantiation.
  static unsigned long gLastOkMs;

  /// This instance's own Location choice, read straight off its own
  /// descriptor rather than cached in a global - same reasoning the original
  /// single-instance wantsTarget() gives: the policy can change under this
  /// module at any check-in, and a cached choice would keep querying the
  /// location an admin just switched away from until the next fetch
  /// happened to re-read it anyway, which re-reading directly makes moot.
  static bool wantsTarget() {
    const int8_t index = Cards::indexOf(id());
    if (index < 0) {
      return false;
    }
    return strcasecmp(Cards::at(static_cast<uint8_t>(index)).location, "target") == 0;
  }

  /// Unsigned subtraction, correct across the millis() rollover at ~49
  /// days - identical to Weather.cpp's/Listings.cpp's own
  /// describeFreshness(), duplicated per the same reasoning those two give
  /// each other's copies.
  static String describeFreshness(unsigned long fetchedAtMs) {
    const unsigned long ageMinutes = (millis() - fetchedAtMs) / 60000UL;
    if (ageMinutes == 0) {
      return "Updated just now";
    }
    if (ageMinutes == 1) {
      return "Updated 1 min ago";
    }
    return String("Updated ") + ageMinutes + " min ago";
  }

  /// Calls the free, parameterised `Forecast::fetch(bool)` above - qualified
  /// explicitly because an unqualified call from inside this nested struct
  /// would resolve to this very member (C++ member-name lookup stops at the
  /// first scope that declares the name, regardless of arity), not the
  /// enclosing namespace's function.
  static void fetch() {
    const bool useTarget = wantsTarget();
    gLast = Forecast::fetch(useTarget);
    gEverFetched = true;
    if (gLast.status == Status::Ok) {
      gLastOkMs = millis();
      Log::printf("[%s] card updated (%s): %u period(s), starting with '%s' %d%s %s", id(),
                  useTarget ? "target" : "home", gLast.count, gLast.periods[0].name.c_str(),
                  gLast.periods[0].temperature, gLast.periods[0].unit.c_str(),
                  gLast.periods[0].shortForecast.c_str());
    }
  }

  /// One item once anything has been fetched, zero before that - the whole
  /// week now draws in a single combined slide (see draw() below), so unlike
  /// the old per-period version of this card there is no longer a count of
  /// pageable items to report. Same "a message is content too" tolerance
  /// Weather's/Listings' own cardItemCount() give their resting and error
  /// states.
  static uint16_t itemCount() { return gEverFetched ? 1 : 0; }

  static void draw(uint16_t) {
    if (gLast.status == Status::Ok) {
      // The strip reads every *other* fetched period - periods[0] is the
      // hero's own day, periods[2]/[4]/[6]/[8] are the next four calendar
      // days - skipping the overnight period NWS always interleaves between
      // two daytime ones. See Display::showForecastCard()'s own remarks for
      // why this stays correct whether the device fetched at 2pm or 2am.
      String dayNames[Display::kMaxForecastStripDays];
      int dayTemperatures[Display::kMaxForecastStripDays];
      String dayUnits[Display::kMaxForecastStripDays];
      String dayConditions[Display::kMaxForecastStripDays];
      uint8_t dayCount = 0;
      for (uint16_t sourceIndex = 0;
           sourceIndex < gLast.count && dayCount < Display::kMaxForecastStripDays;
           sourceIndex += 2) {
        const PeriodInfo& period = gLast.periods[sourceIndex];
        dayNames[dayCount] = period.name;
        dayTemperatures[dayCount] = period.temperature;
        dayUnits[dayCount] = period.unit;
        dayConditions[dayCount] = period.shortForecast;
        dayCount++;
      }

      const PeriodInfo& current = gLast.periods[0];
      Display::showForecastCard(gLast.location, current.isDaytime, current.temperature,
                                current.unit, current.shortForecast, dayNames, dayTemperatures,
                                dayUnits, dayConditions, dayCount, describeFreshness(gLastOkMs));
      return;
    }

    // NotActivated, ProviderDisabled and Empty are resting states - nothing
    // wrong with the device, just nothing configured or nothing resolved
    // yet - shown muted rather than amber, the same isProblem split
    // Weather's and Listings' own cards make.
    const bool isRestingState = gLast.status == Status::NotActivated ||
                                gLast.status == Status::ProviderDisabled ||
                                gLast.status == Status::Empty;
    const String headline = gLast.status == Status::Empty ? "No forecast available right now"
                            : isRestingState               ? "Forecast is not showing yet"
                                                            : "Could not load the forecast";
    Display::showForecastStatus(headline, gLast.message, /*isProblem=*/!isRestingState);
  }

  /// Builds and registers this instance's descriptor. Called once per
  /// instantiation from the static-init block at the bottom of this file.
  static bool registerSelf(int16_t order) {
    Cards::CardSpec spec;
    spec.id = id();
    spec.kind = Cards::Kind::List;
    spec.fetch = &fetch;
    spec.itemCount = &itemCount;
    spec.draw = &draw;
    spec.order = order;
    spec.dwellSeconds = 10;
    return Cards::registerCard(spec);
  }
};

template <int N>
Result Instance<N>::gLast;
template <int N>
bool Instance<N>::gEverFetched = false;
template <int N>
unsigned long Instance<N>::gLastOkMs = 0;

// The one piece of Instance<N> that cannot be written generically - each
// instance's id is a distinct string, not a function of N in any way the
// compiler could derive on its own.
template <>
const char* Instance<1>::id() {
  return kCardId;
}
template <>
const char* Instance<2>::id() {
  return kCardId2;
}
template <>
const char* Instance<3>::id() {
  return kCardId3;
}
template <>
const char* Instance<4>::id() {
  return kCardId4;
}
template <>
const char* Instance<5>::id() {
  return kCardId5;
}

// List, not Interstitial, for all five: each instance takes its own fixed
// slot in the rotation rather than interleaving after every N other cards
// the way the retired standalone weather card did - unrelated to how many
// periods it fetches, which no longer affects itemCount() now that every
// period draws on one combined slide (see Instance<N>::draw()). Order 4
// slots all five right after Listings (3) among list cards; the scheduler's
// own tie-break (Cards.h's `earlier()`) means registration order settles
// ties among the five, which is as arbitrary - and as harmless - as any
// other tie-break would be.
[[maybe_unused]] const bool kRegistered1 = Instance<1>::registerSelf(4);
[[maybe_unused]] const bool kRegistered2 = Instance<2>::registerSelf(4);
[[maybe_unused]] const bool kRegistered3 = Instance<3>::registerSelf(4);
[[maybe_unused]] const bool kRegistered4 = Instance<4>::registerSelf(4);
[[maybe_unused]] const bool kRegistered5 = Instance<5>::registerSelf(4);

}  // namespace

}  // namespace Forecast
