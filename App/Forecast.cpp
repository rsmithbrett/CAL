#include "Forecast.h"

#include <ArduinoJson.h>

#include "Cards.h"
#include "Config.h"
#include "Display.h"
#include "Http.h"
#include "Identity.h"
#include "Log.h"

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

  if (!Http::ready()) {
    result.message = "Cannot verify the service's identity.";
    Log::line("[forecast] TLS setup failed");
    return result;
  }

  const String url =
      String("https://") + Config::kServiceHost + (useTarget ? kPathTarget : kPathHome);
  if (!Http::beginRequest(url)) {
    result.message = "Cannot reach the forecast service.";
    Log::line("[forecast] could not begin request");
    return result;
  }
  HTTPClient& http = Http::client();
  http.setTimeout(Config::kHttpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());

  Log::verbose("[forecast] GET %s", url.c_str());
  const int status = http.GET();
  Log::verbose("[forecast] response status=%d", status);

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
  // Built once and reused for the life of the device - see Aircraft.cpp's own
  // filter for the full reasoning. This site benefits most of the three:
  // Forecast has five card instances, so what used to be five 1KB pool
  // allocate/free pairs in the post-boot fetch burst is now a single resident
  // document shared by all of them.
  static const JsonDocument filter = [] {
    JsonDocument f;
    f["city"] = true;
    f["state"] = true;
    f["postalCode"] = true;
    f["periods"][0]["name"] = true;
    f["periods"][0]["isDaytime"] = true;
    f["periods"][0]["temperature"] = true;
    f["periods"][0]["temperatureUnit"] = true;
    f["periods"][0]["shortForecast"] = true;
    return f;
  }();

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

  // A lightly-summarized response, not the raw body: this endpoint's own
  // filter (see the JsonDocument filter above) already exists to keep the
  // untrimmed response's memory cost off this device (see the README's own
  // account of a 9,194-byte unfiltered response failing to parse on real
  // hardware) - reconstructing that same cost here by buffering the raw body
  // just to log it would undo the reason the filter exists. What was
  // actually parsed is enough to reconstruct what this fetch found.
  // Untagged with a per-instance id on purpose: this free function is shared
  // by all five Instance<N>s (see Instance<N>::fetch() below, which calls
  // straight into it with no instance context of its own), so there is no
  // single card id to name here - each instance's own draw() logs its id
  // alongside the content actually shown.
  Log::verbose("[forecast] response location=%s periods=%u first='%s' %d%s %s",
              result.location.c_str(), static_cast<unsigned>(result.count),
              result.periods[0].name.c_str(), result.periods[0].temperature,
              result.periods[0].unit.c_str(), result.periods[0].shortForecast.c_str());

  return result;
}

namespace {

/// One memoized response, per distinct URL this card can ask for.
///
/// **Why this exists.** `fetch(bool)` has exactly two possible URLs -
/// kPathHome and kPathTarget - so five card instances can produce at most two
/// distinct responses. Without this, all five did their own full HTTPS request
/// plus their own ~1KB JsonDocument pools, meaning three of the five were
/// provably redundant every time. They also fire together: CardManager treats
/// a card that has never fetched as immediately due, and CardSpec::active
/// defaults true, so every fetch-capable card fetches back-to-back in the
/// post-boot burst - the worst possible moment to be doing three unnecessary
/// TLS handshakes and six unnecessary pool allocations.
///
/// Cards.h already argued for exactly this: it is the reason aircraft and
/// listings were denied multi-instance treatment in the first place. Forecast
/// got the treatment anyway because its instances differ in a way theirs do
/// not - and this is the piece that makes that affordable.
struct SharedSlot {
  Result result;
  unsigned long fetchedAtMs = 0;
  bool everFetched = false;
};

/// Indexed by the `useTarget` bool itself: [0] is home, [1] is target.
SharedSlot gShared[2];

/// The response for `useTarget`, fetched only if no recent enough one is
/// already in hand.
///
/// Freshness uses the same kContentRefreshIntervalMs the scheduler uses to
/// decide a card is due, which is what makes this a de-duplicator rather than
/// a second, competing cache policy: instances asking for the same URL within
/// one refresh window share one answer, and once that window passes the next
/// asker refetches for everyone. Note that a failed fetch is memoized too, on
/// purpose - five instances retrying a down service five times over is exactly
/// the storm this exists to prevent, and the retry still happens on the next
/// window.
const Result& sharedFetch(bool useTarget) {
  SharedSlot& slot = gShared[useTarget ? 1 : 0];
  const unsigned long now = millis();

  if (slot.everFetched && (now - slot.fetchedAtMs) < Config::kContentRefreshIntervalMs) {
    Log::verbose("[forecast] reusing the %s response fetched %lu ms ago - no second request",
                 useTarget ? "target" : "home",
                 static_cast<unsigned long>(now - slot.fetchedAtMs));
    return slot.result;
  }

  slot.result = Forecast::fetch(useTarget);
  slot.fetchedAtMs = now;
  slot.everFetched = true;
  return slot.result;
}

}  // namespace

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
    // wantsTarget() is still re-read per instance, every time - the policy can
    // change under this card between fetches, and which of the two URLs THIS
    // instance wants is genuinely per-instance state. Only the response to a
    // given URL is shared.
    const bool useTarget = wantsTarget();
    gLast = sharedFetch(useTarget);
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
      // What is actually on screen this draw, not just what the last fetch
      // found - the two can diverge across a rewind, where this runs again
      // with no fresh fetch behind it. Logged every draw, unlike fetch()'s
      // own printf() summary above which only fires on a successful fetch.
      Log::verbose("[%s] drawing: location=%s hero='%s' %d%s %s dayCount=%u", id(),
                  gLast.location.c_str(), current.name.c_str(), current.temperature,
                  current.unit.c_str(), current.shortForecast.c_str(),
                  static_cast<unsigned>(dayCount));
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
    Log::verbose("[%s] drawing status screen: %s (%s)", id(), headline.c_str(),
                gLast.message.c_str());
    Display::showForecastStatus(headline, gLast.message, /*isProblem=*/!isRestingState);
  }

  /// Re-asserted once per check-in - see Cards.h's StatusFn. Names which of
  /// the two locations this instance is configured for, because with five
  /// instances "forecast3 is refused" is only half an answer: whether it was
  /// asking for home or target is the other half.
  static String status() {
    if (!gEverFetched) {
      return String(wantsTarget() ? "target" : "home") + ", never fetched";
    }
    const String where = wantsTarget() ? "target" : "home";
    switch (gLast.status) {
      case Status::Ok:
        return where + ", ok: " + gLast.location;
      case Status::Empty:
        return where + ", ok: no periods returned";
      case Status::NotActivated:
        return where + ", refused: device not activated";
      case Status::ProviderDisabled:
        return where + ", refused: provider disabled";
      case Status::AuthError:
        return where + ", refused: device secret rejected";
      case Status::NetworkError:
        return where + ", network error: " + gLast.message;
    }
    return where + ", unknown";
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
    spec.status = &status;
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
