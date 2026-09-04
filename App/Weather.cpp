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

// ---------------------------------------------------------------------------
// Log-on-change trackers.
//
// fetchMine() runs on the scheduler's content-refresh timer, every
// Config::kContentRefreshIntervalMs (10 minutes). Anything logged
// unconditionally from in here emits six lines an hour forever, and the
// remote debug stream keeps a bounded buffer - so a chatty steady state
// pushes out exactly the context someone is reading the stream to find.
// Every helper below therefore says something the first time a condition
// starts being true and the first time it stops, and nothing in between.
// Same discipline as Graphic.cpp's noteState() and SunMoon.cpp's
// gLastLoggedSunrise/gLastLoggedSunset.
// ---------------------------------------------------------------------------

bool gWarnedNoTemperature = false;
bool gWarnedNoShortForecast = false;
bool gWarnedNoLocation = false;
const char* gLastLoggedSource = nullptr;

/// Logs `problem` the first time a field goes missing and `recovered` the
/// first time it comes back. Silent on every pass where nothing changed.
void noteFieldHealth(bool isMissing, bool& wasMissing, const char* problem,
                     const char* recovered) {
  if (isMissing == wasMissing) {
    return;
  }
  wasMissing = isMissing;
  Log::line(isMissing ? problem : recovered);
}

/// Picks Target over Home when the owner has one set - the more useful of the
/// two for a device displayed away from the owner's own address - matching
/// UserWeatherResult's own doc comment on which field callers should prefer.
///
/// Which of the two it picked is reported through `whichOut` so the caller can
/// log it. That choice is invisible on the card itself - both render as an
/// ordinary city name - so a device quietly showing the owner's Target city
/// when someone expected their Home city (or the reverse, after a Target is
/// cleared) has no on-screen tell at all. The log line is the only place that
/// distinction is ever recoverable from a deployed device.
JsonVariantConst preferredForecast(JsonVariantConst body, const char** whichOut) {
  JsonVariantConst target = body["target"];
  if (!target.isNull()) {
    *whichOut = "target";
    return target;
  }
  *whichOut = "home";
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

  const char* source = "home";
  JsonVariantConst weatherResult = preferredForecast(doc.as<JsonVariantConst>(), &source);
  if (weatherResult.isNull()) {
    result.message = "No home address is set for this device's owner yet.";
    Log::line("[weather] no home/target address on file");
    return result;
  }

  if (gLastLoggedSource == nullptr || strcmp(gLastLoggedSource, source) != 0) {
    gLastLoggedSource = source;
    Log::printf("[weather] reading the owner's %s address for this device's forecast", source);
  }

  JsonArrayConst periods = weatherResult["periods"].as<JsonArrayConst>();
  if (periods.isNull() || periods.size() == 0) {
    result.message = "No forecast is available yet.";
    Log::line("[weather] no forecast periods in response");
    return result;
  }

  JsonVariantConst current = periods[0];

  // Every one of these three used to fail silently into a plausible-looking
  // card. A missing "temperature" defaulted to 0 and drew as a real reading
  // of zero degrees, which on this card is indistinguishable from a genuine
  // freezing morning; a missing "shortForecast" drew as an empty gap; a city
  // and state the server did not send drew as no location line at all. None
  // of the three left any trace anywhere, on the device or in the stream, so
  // "the weather card looks wrong" was un-diagnosable without a debugger and
  // a packet capture. They still render the same way - inventing a
  // substitute reading would be worse than showing none - but they are no
  // longer quiet about it.
  noteFieldHealth(current["temperature"].isNull(), gWarnedNoTemperature,
                  "[weather] the server sent no temperature - the card will read 0 degrees, "
                  "which is NOT a real reading",
                  "[weather] temperature is being sent again");
  const String shortForecast = String(current["shortForecast"] | "");
  noteFieldHealth(shortForecast.length() == 0, gWarnedNoShortForecast,
                  "[weather] the server sent no shortForecast - the card's condition line "
                  "will be blank",
                  "[weather] shortForecast is being sent again");

  result.status = Status::Ok;
  result.forecast.location = describeLocation(weatherResult);
  result.forecast.temperature = current["temperature"] | 0;
  result.forecast.unit = String(current["temperatureUnit"] | "F");
  result.forecast.shortForecast = shortForecast;

  noteFieldHealth(result.forecast.location.length() == 0, gWarnedNoLocation,
                  "[weather] no city/state/postalCode for this device's address - the card "
                  "will not say where the reading is from",
                  "[weather] the address is resolving to a place name again");
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

/// millis() when gLast last became an Ok result. Drives the card's "Updated N
/// min ago" line, which was previously the hardcoded string "Updated just
/// now" on every single draw - including a redraw of a card fetched twenty
/// minutes earlier, and including reverse navigation into card history, where
/// "just now" was reliably false by construction. A freshness label that is
/// always the same string is worse than none: it reads as a live measurement
/// and is not one.
unsigned long gLastOkMs = 0;

/// Change-gated draw logging. cardDraw() runs every time the card comes round
/// in the rotation, so this must never log per-draw; see the trackers at the
/// top of this file for the same reasoning applied to the fetch half.
Status gLastDrawnStatus = Status::NetworkError;
String gLastDrawnSummary;
bool gEverDrawn = false;

/// Unsigned subtraction, so this stays correct across the millis() rollover at
/// ~49 days rather than reporting a wildly negative age once a device has been
/// up that long. Minutes, because the refresh interval is 10 of them - seconds
/// would be false precision on a card looked at from across a room.
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

/// The three fields that make one reading distinct from another, joined for
/// comparison only - never for display. Built through an explicit String
/// rather than a chain of operator+ in a ternary, because that chain yields
/// StringSumHelper on some ESP32 core versions and mixing it with String in a
/// conditional is exactly the kind of thing that compiles on one core and
/// not the next.
String forecastSummary(const Result& result) {
  if (result.status != Status::Ok) {
    return String("");
  }
  String summary = result.forecast.location;
  summary += "|";
  summary += String(result.forecast.temperature);
  summary += result.forecast.unit;
  summary += "|";
  summary += result.forecast.shortForecast;
  return summary;
}

void cardFetch() {
  const bool hadGoodForecast = gEverFetched && gLast.status == Status::Ok;
  const unsigned long previousOkMs = gLastOkMs;
  // Empty before the first fetch and after any non-Ok one, which is what
  // forecastSummary() already returns for those cases - no guard needed.
  const String previousSummary = forecastSummary(gLast);

  gLast = fetchMine();
  gEverFetched = true;

  if (gLast.status == Status::Ok) {
    gLastOkMs = millis();
    const String summary = forecastSummary(gLast);
    // Only when the reading actually moved. A forecast that is genuinely
    // unchanged between two refreshes is the ordinary case and saying so
    // every ten minutes buys nothing.
    if (summary != previousSummary) {
      Log::printf("[weather] card updated: %s, %d%s, %s", gLast.forecast.location.c_str(),
                  gLast.forecast.temperature, gLast.forecast.unit.c_str(),
                  gLast.forecast.shortForecast.c_str());
    }
    return;
  }

  // A failed refresh REPLACES a good forecast with an error message rather
  // than keeping the reading that was already on the card - so a single
  // network blip turns a working weather card into "Could not load weather"
  // until the next refresh ten minutes later. That is a deliberate existing
  // behaviour (the scheduler treats a bad-news card as a card worth showing,
  // see cardItemCount below), not a bug being fixed here, but it is exactly
  // the kind of silent discard that is impossible to reconstruct after the
  // fact from a device in someone's kitchen. It now says so, with the age of
  // what it threw away.
  if (hadGoodForecast) {
    Log::printf("[weather] refresh failed (%s) - discarding the %lu-minute-old forecast that "
                "was on the card, which now shows the error instead",
                gLast.message.c_str(), (millis() - previousOkMs) / 60000UL);
  }
}

/// One item once anything has been fetched, zero before that. A non-Ok
/// status still counts as one item, not zero: "weather is not showing yet"
/// or "could not load weather" is a message worth putting on screen, and the
/// scheduler's empty-card skipping is meant for cards with genuinely nothing
/// to say - not for cards with bad news.
uint16_t cardItemCount() { return gEverFetched ? 1 : 0; }

/// Says what the card put on screen, but only when that changed since the
/// last draw - this runs on every pass through the rotation.
void noteDrawn(const String& summary) {
  if (gEverDrawn && gLastDrawnStatus == gLast.status && gLastDrawnSummary == summary) {
    return;
  }
  gEverDrawn = true;
  gLastDrawnStatus = gLast.status;
  gLastDrawnSummary = summary;
  Log::printf("[weather] drew: %s", summary.c_str());
}

void cardDraw(uint16_t) {
  if (gLast.status == Status::Ok) {
    const String freshness = describeFreshness(gLastOkMs);
    Display::showWeatherCard(gLast.forecast.location, gLast.forecast.temperature,
                             gLast.forecast.unit, gLast.forecast.shortForecast, freshness);

    // Note what was drawn WITHOUT the freshness string in the summary, even
    // though it is on screen. Freshness ticks over on its own every minute,
    // so folding it in would make every summary differ from the last one and
    // turn this change-gated line back into a per-draw line - the exact
    // steady-state chatter the trackers at the top of this file exist to
    // avoid. Age is already recoverable from the fetch lines.
    //
    // The first branch is the one genuinely blank outcome this card can reach
    // while still believing it succeeded: the server answered 200 with a
    // period carrying neither a temperature nor a condition. The panel then
    // shows a banner, a location and "0 F", which reads as a working card
    // reporting freezing weather rather than as a failure. It gets its own
    // sentence rather than being folded into the ordinary summary, because
    // someone reading the stream for "why does the weather card look wrong"
    // needs to be told, not left to infer it.
    if (gLast.forecast.shortForecast.length() == 0 && gLast.forecast.temperature == 0) {
      noteDrawn("a card with no condition and no temperature - an empty forecast rendered as "
                "0 degrees, NOT real weather");
      return;
    }

    noteDrawn(gLast.forecast.location + ", " + String(gLast.forecast.temperature) +
              gLast.forecast.unit + ", " + gLast.forecast.shortForecast);
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
  const String headline =
      isRestingState ? "Weather is not showing yet" : "Could not load weather";
  Display::showWeatherStatus(headline, gLast.message, /*isProblem=*/!isRestingState);

  // The status screens get the same treatment as the card. Without this, a
  // device sitting on "Could not load weather" for hours produced fetch-side
  // failure lines but nothing at all confirming that this is what a person
  // standing in front of it is actually looking at - and the two can differ,
  // since a card only redraws when the rotation reaches it.
  noteDrawn(headline + " - " + gLast.message);
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
