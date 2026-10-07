#include "Aircraft.h"

#include <ArduinoJson.h>

#include "Assets.h"
#include "Cards.h"
#include "Config.h"
#include "Display.h"
#include "Http.h"
#include "Identity.h"
#include "Log.h"
#include "Maintenance.h"
#include "ProviderStatus.h"

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
    result.serviceUnreachable = true;
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

  if (!Http::ready()) {
    result.message = "Cannot verify the service's identity.";
    Log::line("[aircraft] TLS setup failed");
    return result;
  }

  const String url = String("https://") + Config::kServiceHost + kPath;
  if (!Http::beginRequest(url)) {
    result.message = "Cannot reach the aircraft service.";
    result.serviceUnreachable = true;
    Log::line("[aircraft] could not begin request");
    return result;
  }
  HTTPClient& http = Http::client();
  http.setTimeout(Config::kHttpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());

  Log::verbose("[aircraft] GET %s", url.c_str());
  const int status = http.GET();
  Log::verbose("[aircraft] response status=%d", status);

  if (status == 401) {
    http.end();
    result.status = Status::AuthError;
    result.message = "This display is not signed in to an account. Set it up again from your account page.";
    Log::line("[aircraft] auth rejected (401)");
    return result;
  }

  if (status == 403) {
    const String body = http.getString();
    http.end();
    return parseRefusal(body);
  }

  // 404 is "we do not know where this display is", not a server that is down.
  // The route resolves a position from the display's owner, that owner's home
  // address, and the connecting address in turn, and answers 404 when all
  // three come to nothing - a display with no owner reaches this every time.
  // Named as its own state so the card stops reporting a working server as
  // unreachable, and so the sentence says the one thing somebody can act on.
  if (status == 404) {
    http.end();
    result.status = Status::NoPosition;
    result.message = "We do not know where this display is. Add a home address to the account that holds it.";
    Log::line("[aircraft] no position for this device (404)");
    return result;
  }

  // The service answered and could not help - it is reachable, so the card
  // declines to say anything about the sky rather than blaming the network.
  // Below 500 that is a refusal we have no better name for; at 500 and above
  // it is the server's own fault, and either way the reader can only wait.
  if (status >= 429) {
    http.end();
    result.status = Status::RefreshFailed;
    result.message = "The flight service is busy. This will catch up on its own.";
    Log::printf("[aircraft] service declined, http status=%d", status);
    return result;
  }

  if (status != 200) {
    http.end();
    result.message = "Cannot reach the aircraft service.";
    result.serviceUnreachable = true;
    Log::printf("[aircraft] unexpected http status=%d", status);
    return result;
  }

  // Only the fields this card actually draws are worth keeping in the filter
  // - same rationale as CYD-Dickey's Aircraft.cpp filtering adsb.lol's
  // couple-dozen raw fields down to five, just applied to this server's own
  // AircraftSighting shape. originName/destinationName are now whitelisted
  // alongside their codes - see Display::showAircraftCard()'s remarks for
  // why the card draws a name when one is on file and only falls back to the
  // bare code per side when it isn't.
  // Built once, on first fetch, and reused for the life of the device.
  //
  // A JsonDocument takes a pool block from the heap the moment it holds
  // anything - 1KB on this 32-bit target, ARDUINOJSON_POOL_CAPACITY being 128
  // slots - and this one was built, allocated and freed on every single fetch
  // to hold nothing but a handful of booleans. The keys are string literals,
  // so ArduinoJson links to them rather than copying, and the shape never
  // varies: this document has no per-fetch input at all. Keeping it resident
  // trades ~1KB of permanent heap for zero allocation churn on a device whose
  // scarce resource is contiguous blocks rather than total bytes - the same
  // trade Display.cpp's gFileBuffer and PNG decode scratch already make, for
  // the same measured reason.
  //
  // Function-local static, so it is constructed on first use rather than
  // during static init where the heap is in no state to be relied on.
  //
  // THE FILTER IS NOT A DOCUMENTATION DETAIL, IT IS THE READ ITSELF. An
  // un-whitelisted key is dropped during deserialization and never reaches
  // `doc` at all, so `doc["status"]` on a filter without a `status` entry is
  // indistinguishable from a server that never sent one - a silent, permanent
  // fallback to "the refresh was fine" no matter what the server does. That is
  // not hypothetical: it is why `lastRefreshError` was invisible to this card
  // for its whole life, and why Listings.cpp calls the filter the load-bearing
  // half of the same change. Adding a field to this card means adding it here
  // in the same edit.
  static const JsonDocument filter = [] {
    JsonDocument f;
    f["radiusMiles"] = true;
    // A top-level sibling of the array, not part of it, so it gets its own
    // entry. See the empty-list branch below for what it decides.
    f["status"] = true;
    f["aircraft"][0]["callsign"] = true;
    f["aircraft"][0]["altitudeFeet"] = true;
    f["aircraft"][0]["speedKnots"] = true;
    f["aircraft"][0]["headingDegrees"] = true;
    f["aircraft"][0]["distanceMiles"] = true;
    f["aircraft"][0]["airlineCode"] = true;
    f["aircraft"][0]["airlineName"] = true;
    f["aircraft"][0]["airlineLogoAssetId"] = true;
    f["aircraft"][0]["originCode"] = true;
    f["aircraft"][0]["originName"] = true;
    f["aircraft"][0]["destinationCode"] = true;
    f["aircraft"][0]["destinationName"] = true;
    // The military four. Whitelisted like the rest: the filter keeps only the fields
    // this card draws, so a key missing from it is a key the parse below cannot see
    // however faithfully the server sends it.
    f["aircraft"][0]["isMilitary"] = true;
    f["aircraft"][0]["militaryBranch"] = true;
    f["aircraft"][0]["aircraftType"] = true;
    f["aircraft"][0]["registration"] = true;
    return f;
  }();

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

  // THE SIGNAL, AND THE ONE FIELD THIS CARD READS TO GET IT. `ProviderStatus`
  // on the server - a closed vocabulary derived on the record, meant to be read
  // by a machine. See ProviderStatus.h for what each value means and why a card
  // reads this rather than the operator's sentence sitting next to it.
  //
  // WHY THERE IS NO `lastRefreshError` FALLBACK HERE, unlike Listings.cpp.
  // That card reads the old field's PRESENCE when `status` is absent, because
  // it already had correct behaviour built on that inference and deleting it
  // would regress a shipped card against a server predating the strip. This
  // card never read the field at all - it was on the wire and was never
  // whitelisted - so there is no behaviour to preserve, and adding the read now
  // would newly teach a card to treat an operator's prose as a protocol
  // element, in the very change that exists to stop doing that. It would also
  // pull that prose into the parsed document on a card that has never had it
  // there, which is leak surface bought for a purely transitional benefit.
  //
  // The consequence is stated plainly rather than buried: against a pre-strip
  // server this card behaves exactly as it does today, empty-sky claim and all.
  // It becomes correct the moment the server sends `status`, and draws nothing
  // worse in the meantime.
  const char* statusText = doc["status"] | "";
  const ProviderStatus::Value wire = ProviderStatus::parse(statusText);

  // The one derived fact. Everything downstream reads this boolean.
  bool refreshFailed = false;
  switch (wire) {
    case ProviderStatus::Value::Ok:
      refreshFailed = false;
      break;
    // "Nothing was attempted" is not "an attempt failed". The branch below
    // rests on NotConfigured before the aircraft array is ever consulted, so
    // this value never reaches a claim about the sky either way.
    case ProviderStatus::Value::NotConfigured:
      refreshFailed = false;
      break;
    case ProviderStatus::Value::Stale:
    case ProviderStatus::Value::Unavailable:
    case ProviderStatus::Value::Unrecognized:
      refreshFailed = true;
      break;
    // No status on this payload and, per the reasoning above, nothing else to
    // consult. Reads as "the refresh was fine", which is this card's behaviour
    // today and the only reading that does not start calling a genuinely empty
    // sky a failure on every server currently deployed.
    case ProviderStatus::Value::Absent:
      refreshFailed = false;
      break;
  }

  Log::verbose("[aircraft] status='%s' - %s", strlen(statusText) > 0 ? statusText : "(absent)",
               ProviderStatus::describe(wire));

  // A resting state, checked before the array is ever looked at - an account
  // with no provider on file should read as "not set up", never as "nothing
  // overhead". This card has no `isConfigured` of its own, so `status` is the
  // only thing that can say so.
  if (wire == ProviderStatus::Value::NotConfigured) {
    result.status = Status::NotConfigured;
    // A literal chosen for whoever this panel hangs in front of, never the
    // server's own words - the same contract Result::message carries on every
    // other card, and the one the listings card had to be taught after it put
    // a vendor's sign-up instructions on a kitchen wall.
    result.message = "Aircraft tracking is not set up for this home yet.";
    Log::printf("[aircraft] NOT CONFIGURED - resting; neither an empty sky nor a failed refresh, "
                "and the aircraft array was not consulted");
    return result;
  }

  JsonArrayConst aircraft = doc["aircraft"].as<JsonArrayConst>();
  if (aircraft.isNull() || aircraft.size() == 0) {
    // AN EMPTY LIST IS NOT ONE FACT, IT IS TWO - identical in shape to the
    // listings card's own empty-array split, and worse here in one respect: an
    // empty sky is PLAUSIBLE far more often than an empty housing market, so
    // the false version of this claim is much less likely to be questioned by
    // whoever reads it. "No aircraft within 10 mi right now" is a flat
    // statement about the sky, and until now this card made it whether or not
    // anybody had actually looked.
    //
    // Deliberately NOT serviceUnreachable. That flag means "this device could
    // not reach OUR server", which is what a declared maintenance window
    // explains; this is our server answering perfectly well about an upstream
    // feed it could not reach. None of the ProviderStatus values means our
    // server is unreachable - every one arrives on a well-formed 200 - so
    // nothing read out of that field may ever raise it, and failureText() keeps
    // passing this message through untouched.
    if (refreshFailed) {
      result.status = Status::RefreshFailed;
      // Names the cause, where the headline names the outcome, so the two
      // lines carry different information. No claim about the sky, and no
      // count implied.
      result.message = "The flight data service is not answering right now.";
      Log::printf("[aircraft] empty list AND a failed refresh -> RefreshFailed; Empty NOT taken - "
                  "nothing here licenses a claim about the sky. serviceUnreachable stays false: "
                  "our server answered, the upstream feed did not");
      return result;
    }
    result.status = Status::Empty;
    char buffer[48];
    snprintf(buffer, sizeof(buffer), "No aircraft within %.0f mi right now.", result.radiusMiles);
    result.message = String(buffer);
    // The one branch on this card that makes a positive claim about the sky,
    // and until now the only one that logged nothing at all. It says so now,
    // and says what it relied on to be allowed to.
    Log::printf("[aircraft] empty list and a refresh not reported as failed -> Empty; "
                "RefreshFailed NOT taken - drawn as a real statement about the sky within %.0f mi",
                result.radiusMiles);
    return result;
  }

  // Sightings AND a failed refresh - `status: "Stale"`, the value that case
  // exists to name. The server is serving last-known-good rows while its
  // refresh fails behind them. Drawing real sightings beats drawing a warning,
  // exactly as the listings card decides for its own cached rows, so the screen
  // is left alone and only the stream is told.
  if (refreshFailed) {
    Log::printf("[aircraft] serving %u cached sighting(s) behind a failed refresh - drawing them "
                "rather than a warning, and NOT taking RefreshFailed",
                static_cast<unsigned>(aircraft.size()));
  }

  // ELEMENT 0 IS THE NEAREST, AND A MILITARY SIGHTING OUTRANKS IT.
  //
  // The server sends the list in distance order and keeps a place for the nearest
  // military aircraft even when the eight-aircraft cap would have dropped it, which is
  // the whole reason one ever reaches this device. Drawing element 0 regardless would
  // spend that place on a sighting nothing ever shows: this card features exactly one
  // aircraft, and an ordinary airliner is what it features every other minute of the day.
  //
  // Still the NEAREST military one - the list is ordered, and this takes the first match.
  // Falls back to element 0 when there is none, which is every ordinary response.
  JsonVariantConst nearest = aircraft[0];
  for (JsonVariantConst candidate : aircraft) {
    if (candidate["isMilitary"].as<bool>()) {
      nearest = candidate;
      break;
    }
  }

  result.status = Status::Ok;
  result.nearest.callsign = String((const char*)(nearest["callsign"] | "UNKNOWN"));
  result.nearest.altitudeFeet = nearest["altitudeFeet"] | 0;
  result.nearest.speedKnots = nearest["speedKnots"] | 0.0;
  result.nearest.headingDegrees = nearest["headingDegrees"] | 0.0;
  result.nearest.distanceMiles = nearest["distanceMiles"] | 0.0;
  // `| ""` reads a JSON null exactly the same as a field an older server
  // never sends at all - both mean "nothing here" to this client, and the
  // contract deliberately writes null rather than omitting the key, so both
  // shapes have to land on the same empty String regardless.
  result.nearest.airlineCode = String((const char*)(nearest["airlineCode"] | ""));
  result.nearest.airlineName = String((const char*)(nearest["airlineName"] | ""));
  result.nearest.airlineLogoAssetId = String((const char*)(nearest["airlineLogoAssetId"] | ""));
  result.nearest.originCode = String((const char*)(nearest["originCode"] | ""));
  result.nearest.originName = String((const char*)(nearest["originName"] | ""));
  result.nearest.destinationCode = String((const char*)(nearest["destinationCode"] | ""));
  result.nearest.destinationName = String((const char*)(nearest["destinationName"] | ""));
  // Absent on every civil aircraft and on every server old enough to predate the fields,
  // which land on the same empty String for the reason above. The flag reads false in
  // both cases, which is what "not a military sighting" means to this card.
  result.nearest.isMilitary = nearest["isMilitary"].as<bool>();
  result.nearest.militaryBranch = String((const char*)(nearest["militaryBranch"] | ""));
  result.nearest.aircraftType = String((const char*)(nearest["aircraftType"] | ""));
  result.nearest.registration = String((const char*)(nearest["registration"] | ""));

  // A lightly-summarized response rather than the raw body - same filtering
  // reasoning as Forecast::fetch()'s own verbose line: this endpoint's
  // JsonDocument filter above already exists to keep only the fields this
  // card draws in memory, and buffering the raw body just to log it would
  // undo that.
  Log::verbose("[aircraft] response radiusMiles=%.0f callsign=%s alt=%dft dist=%.1fmi",
              result.radiusMiles, result.nearest.callsign.c_str(), result.nearest.altitudeFeet,
              result.nearest.distanceMiles);

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

/// millis() when gLast last became an Ok result - same field, same reasoning,
/// same fix as Weather.cpp's gLastOkMs: "Updated just now" was previously
/// hardcoded on every draw, including a redraw of a sighting fetched minutes
/// earlier and reverse navigation into card history, where it was false by
/// construction.
unsigned long gLastOkMs = 0;

/// Unsigned subtraction, correct across the millis() rollover at ~49 days -
/// identical to Weather.cpp's describeFreshness(), duplicated rather than
/// shared because the two cards' Result types are unrelated and a shared
/// helper would need a third file just to hold one function used twice.
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

/// This card's current fetch state, re-asserted to the debug stream once per
/// check-in - see Cards.h's StatusFn for why change-only logging was not
/// enough. Names the refusal reason rather than just "not ok": "which of the
/// six ways can this be failing" is the entire question an admin is asking.
String cardStatus() {
  if (!gEverFetched) {
    return "never fetched";
  }
  switch (gLast.status) {
    case Status::Ok:
      return String("ok, nearest ") + gLast.nearest.callsign + " at " +
             String(gLast.nearest.distanceMiles, 1) + " mi";
    case Status::Empty:
      return String("ok, nothing within ") + String(gLast.radiusMiles, 0) + " mi";
    // Distinct from Empty on purpose, and this line is where an admin sees the
    // difference: "nothing within 10 mi" is an answer, "could not check" is the
    // absence of one. Carries no reason - that is an operator diagnostic and
    // lives on the server's own /diag/providers, never on a device. See
    // Listings::Result::message for the full argument.
    case Status::RefreshFailed:
      return "upstream refresh failed (reason is on the server, not the device)";
    case Status::NotConfigured:
      return "resting: no aircraft provider on file";
    case Status::NoPosition:
      return "resting: no position for this device";
    case Status::NotActivated:
      return "refused: device not activated";
    case Status::ProviderDisabled:
      return "refused: provider disabled";
    case Status::AuthError:
      return "refused: device secret rejected";
    case Status::NetworkError:
      return String("network error: ") + gLast.message;
  }
  return "unknown";
}

void cardFetch() {
  gLast = fetchMine();
  gEverFetched = true;
  if (gLast.status == Status::Ok) {
    gLastOkMs = millis();

    // Belongs on the fetch path, not draw: ensureCached() fetches on a miss,
    // and Cards.h's whole fetch/draw split exists so stepping backwards
    // through the rotation is never a network operation - see Graphic.cpp's
    // own remarks on the same split. A blank airlineLogoAssetId is the
    // ordinary case (no logo on file for this row yet) and skips the call
    // entirely rather than asking Assets to cache an empty id.
    if (gLast.nearest.airlineLogoAssetId.length() > 0) {
      const bool logoReady = Assets::ensureCached(gLast.nearest.airlineLogoAssetId);
      Log::printf("[aircraft] logo %s for '%s': %s", logoReady ? "cached" : "unavailable",
                  gLast.nearest.airlineCode.c_str(), gLast.nearest.airlineLogoAssetId.c_str());
    }

    // Logged the same way it's drawn: per-side name-with-code-fallback, not
    // an all-or-nothing switch, so this line tells you exactly what
    // showAircraftCard() is about to put on screen rather than a summary
    // that could disagree with it. See that function's own remarks for why
    // the fallback is per-side.
    const String originSummary =
        gLast.nearest.originName.length() > 0 ? gLast.nearest.originName : gLast.nearest.originCode;
    const String destinationSummary = gLast.nearest.destinationName.length() > 0
        ? gLast.nearest.destinationName
        : gLast.nearest.destinationCode;
    String routeSummary = "none on file";
    if (originSummary.length() > 0) {
      routeSummary = destinationSummary.length() > 0 ? (originSummary + "->" + destinationSummary)
                                                       : ("from " + originSummary + " only");
    }
    Log::printf(
        "[aircraft] card updated: %s (%s) alt=%dft speed=%.0fkts heading=%.0f dist=%.1fmi route=%s",
        gLast.nearest.callsign.c_str(),
        gLast.nearest.airlineName.length() > 0 ? gLast.nearest.airlineName.c_str() : "no airline match",
        gLast.nearest.altitudeFeet, gLast.nearest.speedKnots, gLast.nearest.headingDegrees,
        gLast.nearest.distanceMiles, routeSummary.c_str());
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
    // What is actually on screen this draw, not just what the last fetch
    // found - the two can diverge across a rewind, where this runs again
    // with no fresh fetch behind it. cardFetch()'s own printf() summary
    // above only fires on a successful fetch, not on every draw.
    Log::verbose(
        "[aircraft] drawing: %s alt=%dft speed=%.0fkts heading=%.0f dist=%.1fmi logo=%s",
        gLast.nearest.callsign.c_str(), gLast.nearest.altitudeFeet, gLast.nearest.speedKnots,
        gLast.nearest.headingDegrees, gLast.nearest.distanceMiles,
        gLast.nearest.airlineLogoAssetId.length() > 0 ? gLast.nearest.airlineLogoAssetId.c_str()
                                                        : "(none)");
    // THE OPERATOR'S NAME SLOT CARRIES THE SERVICE, because that is what it is for: the
    // line under the callsign says who is flying this aircraft, and for a military
    // sighting that is "U.S. Navy" rather than an airline. The airframe joins it when the
    // provider named one, since "U.S. Navy - MH-60 Seahawk" is the whole answer a
    // household wants and the slot already goes through layoutText, which bounds it.
    //
    // No new parameter and no geometry change. A military sighting has no filed route, so
    // the two airport lines are empty and the card has the room.
    String operatorName = gLast.nearest.airlineName;
    if (gLast.nearest.isMilitary && gLast.nearest.militaryBranch.length() > 0) {
      operatorName = gLast.nearest.militaryBranch;
      if (gLast.nearest.aircraftType.length() > 0) {
        operatorName += " - ";
        operatorName += gLast.nearest.aircraftType;
      }
    }

    Display::showAircraftCard(gLast.nearest.callsign, operatorName,
                              gLast.nearest.altitudeFeet, gLast.nearest.speedKnots,
                              gLast.nearest.headingDegrees, gLast.nearest.distanceMiles,
                              gLast.nearest.originCode, gLast.nearest.destinationCode,
                              gLast.nearest.originName, gLast.nearest.destinationName,
                              describeFreshness(gLastOkMs));

    // Drawn after showAircraftCard(), not by it - same module boundary
    // Graphic.cpp already keeps with Display.cpp: whoever holds the asset id
    // draws the picture, Display.cpp only ever decides where things go (see
    // aircraftLogoZone()). Never fetches - this is the draw path, and a
    // logo that hasn't finished caching this cycle simply doesn't appear
    // this cycle rather than blocking the card on the network.
    if (gLast.nearest.airlineLogoAssetId.length() > 0) {
      int16_t x, y, w, h;
      Display::aircraftLogoZone(x, y, w, h);
      Assets::drawCachedInRect(gLast.nearest.airlineLogoAssetId, x, y, w, h);
    }
    return;
  }

  // Empty (fetch worked, nothing in range right now) and the two resting
  // states read as ordinary/muted; auth and network trouble read amber -
  // same isProblem split Weather's card makes, just with a third muted case
  // this card has and weather doesn't.
  // RefreshFailed joins the muted set rather than the amber one. Amber says
  // "something is wrong with this device" and nothing is: the panel, the
  // network and our server are all fine - an upstream feed our server talks to
  // is not. Same reasoning, and the same resting-vs-amber split, that the
  // listings card applies to its own RefreshFailed; amber in a kitchen for a
  // fault nobody in that kitchen can fix teaches its reader to ignore amber.
  const bool isRestingState = gLast.status == Status::NotActivated ||
                              gLast.status == Status::ProviderDisabled ||
                              gLast.status == Status::NotConfigured ||
                              gLast.status == Status::RefreshFailed ||
                              gLast.status == Status::Empty;
  // Empty and RefreshFailed get DIFFERENT headlines, which is the whole point:
  // one states the sky is clear, the other declines to state anything about it.
  const String headline = gLast.status == Status::Empty  ? "Nothing overhead right now"
                          : gLast.status == Status::RefreshFailed
                              ? "Couldn't check overhead just now"
                          : isRestingState ? "Aircraft overhead is not showing yet"
                                           : "Could not load aircraft data";
  // Resolved on this draw rather than at fetch time - see Forecast.cpp's
  // identical call site and Maintenance.h. The log line keeps the raw message on
  // purpose; the debug stream wants the fault, not the reassurance.
  const String detail = Maintenance::failureText(gLast.message, gLast.serviceUnreachable);
  Log::verbose("[aircraft] drawing status screen: %s (%s)", headline.c_str(),
              gLast.message.c_str());
  Display::showAircraftStatus(headline, detail, /*isProblem=*/!isRestingState);
}

/// Which aircraft was overhead when somebody pressed - see Cards::DescribeFn.
///
/// Callsign, route and distance. The callsign identifies the flight to anyone
/// who looks it up afterwards; the route is what makes it mean something to a
/// reader who will not. Distance is included here, unlike on the listings card,
/// because for an aircraft it is the point of the press - "there was a plane two
/// miles away" is the observation, where a listing's distance from the device is
/// incidental to the house.
///
/// Names fall back to codes per side independently, matching how the card itself
/// draws its route: a flight with one known airport name and one unknown reads
/// better half-resolved than not at all.
String cardDescribe(uint16_t) {
  if (gLast.status != Status::Ok || gLast.nearest.callsign.length() == 0) {
    return String();
  }

  const String origin =
      gLast.nearest.originName.length() > 0 ? gLast.nearest.originName : gLast.nearest.originCode;
  const String destination = gLast.nearest.destinationName.length() > 0
                                 ? gLast.nearest.destinationName
                                 : gLast.nearest.destinationCode;

  String summary = gLast.nearest.callsign;
  if (origin.length() > 0 || destination.length() > 0) {
    summary += " - " + (origin.length() > 0 ? origin : String("?")) + " to " +
               (destination.length() > 0 ? destination : String("?"));
  }
  summary += " - " + String(gLast.nearest.distanceMiles, 1) + " mi away";
  return summary;
}

[[maybe_unused]] const bool kRegistered = [] {
  Cards::CardSpec spec;
  spec.id = "aircraft";
  spec.kind = Cards::Kind::List;
  spec.fetch = cardFetch;
  spec.itemCount = cardItemCount;
  spec.describe = cardDescribe;
  spec.draw = cardDraw;
  spec.isNotable = cardIsNotable;
  spec.status = cardStatus;
  spec.order = 2;
  spec.dwellSeconds = 8;
  spec.notableDwellSeconds = 20;
  return Cards::registerCard(spec);
}();

}  // namespace

}  // namespace Aircraft
