#include "Listings.h"

#include <ArduinoJson.h>

#include "Cards.h"
#include "Config.h"
#include "Display.h"
#include "Http.h"
#include "Identity.h"
#include "Log.h"
#include "Maintenance.h"

namespace Listings {
namespace {

constexpr const char* kPath = "/api/mylistings/mine";

// Identical shape and reasoning to Weather.cpp's/Aircraft.cpp's parseRefusal
// - the same ContentProviderGate produces this 403 body for every
// device-facing content route, listings included (see ContentProviderGate.cs).
Result parseRefusal(const String& body) {
  Result result;
  result.status = Status::NetworkError;

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    result.message = "Cannot reach the listings service.";
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
  Log::printf("[listings] refused (%s): %s", reason, result.message.c_str());
  return result;
}

String describeMarket(const String& cityState) {
  return cityState.length() > 0 ? (" near " + cityState) : String("");
}

/// How much of the server's lastRefreshError to retain. The column is
/// varchar(1000) and this string lives in gLast for as long as the failure
/// does, which on a device whose scarce resource is contiguous heap is not
/// free. Every error the server actually produces identifies itself in its
/// first clause - "Monthly RentCast request budget exhausted", "Could not
/// geocode postal code", the HTTP status of a rejected key - so the head is the
/// part worth carrying and the tail is remediation prose for a web page.
constexpr size_t kMaxRefreshErrorChars = 120;

String retainRefreshError(const char* serverText) {
  String text(serverText);
  if (text.length() > kMaxRefreshErrorChars) {
    text.remove(kMaxRefreshErrorChars);
    text += "...";
  }
  return text;
}

/// The server's device-facing `status` vocabulary - `ProviderStatus` in
/// DiscoverAroundMe.SharedKernel.Content, serialized by name in PascalCase
/// (`"Ok"`, `"NotConfigured"`, `"Stale"`, `"Unavailable"`) rather than in the
/// camelCase the property names around it use, to match the other enum already
/// on the wire in a sibling payload.
///
/// It exists because `lastRefreshError` is prose written for an operator and
/// this card was reading it as a SIGNAL. The server derives this value on the
/// record from state it already has (is a key configured, is there a live
/// error, is there anything cached) rather than authoring it at each failure
/// site, so it cannot drift out of agreement with the diagnostic it replaces.
///
/// Two of the six values are this firmware's, not the server's, and they are
/// deliberately distinct from each other:
///
///   - `Absent` - a 200 from a server that does not send the field yet. Every
///     server this fleet talks to, until the strip below ships. This is the
///     one value that licenses reading the old signal.
///   - `Unrecognized` - a value a later server added that this build has never
///     heard of. NOT the same situation as `Absent`: a newer server said
///     something specific, and `lastRefreshError` will already be gone from
///     its payload, so there is nothing older to fall back to. Treated as a
///     failed refresh, because that is the reading which never invents a claim
///     about somebody's property market.
enum class WireStatus : uint8_t {
  Absent,
  Ok,
  NotConfigured,
  Stale,
  Unavailable,
  Unrecognized,
};

WireStatus parseWireStatus(const char* text) {
  if (text == nullptr || strlen(text) == 0) {
    return WireStatus::Absent;
  }
  if (strcmp(text, "Ok") == 0) {
    return WireStatus::Ok;
  }
  if (strcmp(text, "NotConfigured") == 0) {
    return WireStatus::NotConfigured;
  }
  if (strcmp(text, "Stale") == 0) {
    return WireStatus::Stale;
  }
  if (strcmp(text, "Unavailable") == 0) {
    return WireStatus::Unavailable;
  }
  return WireStatus::Unrecognized;
}

/// For the debug stream only - never drawn, and never compared against. The
/// names double as the reason each value means what it does, because the reader
/// of this line is somebody trying to work out why a card said what it said.
const char* describeWireStatus(WireStatus wire) {
  switch (wire) {
    case WireStatus::Absent:
      return "no status field on this payload - a server that predates the field";
    case WireStatus::Ok:
      return "the server's refresh succeeded; its listings are current";
    case WireStatus::NotConfigured:
      return "no provider credential on file, so nothing was ever attempted";
    case WireStatus::Stale:
      return "refresh failed, last-known-good listings still available";
    case WireStatus::Unavailable:
      return "refresh failed with nothing cached to fall back on";
    case WireStatus::Unrecognized:
      return "a status value this build does not know";
  }
  return "unknown";
}

}  // namespace

Result fetchMine() {
  Result result;

  if (!Http::ready()) {
    result.message = "Cannot verify the service's identity.";
    Log::line("[listings] TLS setup failed");
    return result;
  }

  const String url = String("https://") + Config::kServiceHost + kPath;
  if (!Http::beginRequest(url)) {
    result.message = "Cannot reach the listings service.";
    result.serviceUnreachable = true;
    Log::line("[listings] could not begin request");
    return result;
  }
  HTTPClient& http = Http::client();
  http.setTimeout(Config::kHttpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());

  Log::verbose("[listings] GET %s", url.c_str());
  const int status = http.GET();
  Log::verbose("[listings] response status=%d", status);

  if (status == 401) {
    http.end();
    result.status = Status::AuthError;
    result.message = "Cannot verify this device. Contact support.";
    Log::line("[listings] auth rejected (401)");
    return result;
  }

  if (status == 403) {
    const String body = http.getString();
    http.end();
    return parseRefusal(body);
  }

  // Everything else non-200 - including the 404 GetListingsForDeviceAsync
  // returns when the owner has no Target address on file at all - collapses
  // to the same generic message Weather.cpp's fetchMine() gives its own
  // non-200/401/403 case. There is no richer "no address set" status to
  // report here the way Weather.cpp gets from a 200 body with no home/target
  // key at all: MyListingsEndpoints' "mine" route 404s outright when
  // ResolveTargetZipAsync finds nothing, rather than answering 200 with an
  // empty result the way weather's cascade does. Matching Weather.cpp's own
  // handling exactly rather than inventing a fourth status value for a case
  // this card cannot tell apart from ordinary network trouble anyway.
  if (status != 200) {
    http.end();
    result.message = "Cannot reach the listings service.";
    result.serviceUnreachable = true;
    Log::printf("[listings] unexpected http status=%d", status);
    return result;
  }

  // Only the fields this card actually draws are worth keeping in the filter
  // - same rationale as Aircraft.cpp's own filter. A single index anywhere
  // inside the "listings" array (ArduinoJson's own filter semantics) applies
  // to every element, not just index 0, so this keeps every listing's fields
  // for every element the array actually has. fetchedAtUtc and postalCode are
  // deliberately left out of the filter entirely - freshness is computed
  // client-side from gLastOkMs, the same convention Weather.cpp/Aircraft.cpp
  // already keep, and postalCode has no use on this card once cityState is
  // available. status/isConfigured/lastRefreshError are top-level siblings of
  // the array, not part of it, so they get their own filter entries.
  //
  // THE FILTER IS NOT A DOCUMENTATION DETAIL, IT IS THE READ ITSELF. An
  // un-whitelisted key is dropped during deserialization and never reaches
  // `doc` at all, so `doc["status"]` on a filter without a `status` entry is
  // indistinguishable from a server that never sent one - a silent, permanent
  // fallback to the old inference below no matter what the server does. Adding
  // a field to this card means adding it here in the same edit.
  //
  // Built once and reused for the life of the device - see Aircraft.cpp's own
  // filter for the full reasoning. Short version: a JsonDocument takes a 1KB
  // heap pool block the moment it holds anything, this one's contents never
  // vary, and re-taking that block on every fetch was pure churn on a device
  // whose scarce resource is contiguous blocks.
  static const JsonDocument filter = [] {
    JsonDocument f;
    f["city"] = true;
    f["state"] = true;
    f["status"] = true;
    f["isConfigured"] = true;
    f["lastRefreshError"] = true;
    f["listings"][0]["address"] = true;
    f["listings"][0]["propertyType"] = true;
    f["listings"][0]["price"] = true;
    f["listings"][0]["bedrooms"] = true;
    f["listings"][0]["bathrooms"] = true;
    f["listings"][0]["squareFootage"] = true;
    f["listings"][0]["daysOnMarket"] = true;
    f["listings"][0]["distanceMiles"] = true;
    // The one whitelisted field this card never draws. It rides along so a
    // button press can carry it into the press log and the email - see
    // ListingInfo::mlsNumber and cardDescribe() below.
    f["listings"][0]["mlsNumber"] = true;
    return f;
  }();

  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    result.message = "The listings service sent something unreadable.";
    Log::line("[listings] response was not valid JSON");
    return result;
  }

  const char* city = doc["city"] | "";
  const char* state = doc["state"] | "";
  if (strlen(city) > 0 && strlen(state) > 0) {
    result.cityState = String(city) + ", " + String(state);
  } else if (strlen(city) > 0) {
    result.cityState = String(city);
  }

  // WHY THERE ARE TWO SIGNALS HERE AND NOT ONE.
  //
  // Every branch below needs one fact: did the server's own refresh from
  // RentCast produce fresh data, or not? There are two fields that can answer
  // it, and which one arrives depends on how new the server is.
  //
  // `status` is the answer given deliberately - a closed vocabulary meant to be
  // read by a machine (see WireStatus above).
  //
  // `lastRefreshError` is the answer read by accident. It is the operator's
  // sentence: a rejected key, an exhausted monthly budget, an unresolvable
  // postal code, a bare exception message (MyListingsService.FailAsync). Its
  // mere PRESENCE was this card's only way of telling "the market is empty"
  // from "we never found out", which made a field written for a human into a
  // load-bearing protocol element - and that field is being removed from every
  // device-facing payload precisely because it is prose: one of those sentences
  // ("RentCast API key is not configured. Sign up at rentcast.io and set
  // MyListings:ApiKey.") was rendered onto a real household's kitchen wall.
  // The server marks it [OperatorDiagnostic] and DeviceJsonResult strips every
  // marked property, so on a stripped payload the key is not null - it is
  // absent.
  //
  // So both are read, and `status` wins whenever it is there. A device must
  // behave correctly on a payload carrying either field, or both: the fleet
  // runs v2026.09.14.0003 today and talks to servers on both sides of the
  // strip, sometimes within one rolling deploy.
  //
  // WHEN THE FALLBACK CAN GO. Delete the `lastError`/`hasRefreshError` pair,
  // `retainRefreshError()`, the `f["lastRefreshError"]` filter entry,
  // `Result::refreshError` and its two readers in cardStatus() once no server
  // this fleet can reach predates the strip. That is a server-side fact, not a
  // firmware one: once the strip is deployed everywhere, `hasRefreshError` is
  // permanently false and the `WireStatus::Absent` arm below is dead code. The
  // firmware half is then a pure deletion with no behaviour change - nothing
  // else reads either name.
  const char* statusText = doc["status"] | "";
  const WireStatus wire = parseWireStatus(statusText);

  const char* lastError = doc["lastRefreshError"] | "";
  const bool hasRefreshError = strlen(lastError) > 0;
  if (hasRefreshError) {
    result.refreshError = retainRefreshError(lastError);
  }

  // The one derived fact, and the only place the two wire shapes are reconciled.
  // Everything downstream reads this boolean and never looks at either field
  // again, so there is exactly one line to delete when the fallback goes.
  bool refreshFailed = false;
  switch (wire) {
    case WireStatus::Ok:
      refreshFailed = false;
      break;
    // "Nothing was attempted" is not "an attempt failed". The branch below
    // rests on NotConfigured before the listings array is ever consulted, so
    // this value never reaches a market claim either way.
    case WireStatus::NotConfigured:
      refreshFailed = false;
      break;
    case WireStatus::Stale:
    case WireStatus::Unavailable:
    case WireStatus::Unrecognized:
      refreshFailed = true;
      break;
    // The pre-strip wire shape, and the ONLY arm that consults the old signal.
    case WireStatus::Absent:
      refreshFailed = hasRefreshError;
      break;
  }

  // Both raw signals, what was concluded, and which field did the concluding -
  // logged before any branch is taken, because "why did this card say that" is
  // answered here and a deployed device has no other diagnostic channel. The
  // signal that did NOT decide is named rather than omitted: on a stripped
  // payload "lastRefreshError absent" is the expected, correct state and must
  // not read as a missing value somebody should go hunting for.
  //
  // Two lines rather than one because Log's scratch buffer is 256 bytes and
  // truncates past it - a diagnostic that loses its own tail is worse than two
  // lines.
  Log::verbose("[listings] status='%s' - %s", strlen(statusText) > 0 ? statusText : "(absent)",
               describeWireStatus(wire));
  Log::verbose("[listings] lastRefreshError %s; refresh treated as %s, decided by %s",
               hasRefreshError ? "present" : "absent", refreshFailed ? "FAILED" : "succeeded",
               wire == WireStatus::Absent
                   ? "lastRefreshError's presence (FALLBACK: no status on this payload)"
                   : "status (lastRefreshError not consulted)");

  // A first-class resting state, not an error - see ListingsResult on the
  // server. Checked before ever looking at the listings array: an
  // unconfigured account still gets served whatever stale listings happen to
  // be cached, and this card should read that the same way the server itself
  // treats it - as "not set up yet", not as "nothing nearby".
  //
  // `isConfigured` is NOT stripped - it is not operator prose, it is a boolean,
  // and the server asserts it present on the device payload (see
  // DeviceFacingPayloadTests' "isConfigured":false assertion on the no-key
  // case). So this branch needs no change for the new wire shape and gets
  // none. `status == NotConfigured` is read alongside it only as a second lock:
  // the two cannot disagree on a well-formed payload, because the server
  // derives NotConfigured from `!IsConfigured` before it looks at anything
  // else - but the `| true` default below would wave a payload that somehow
  // carried neither straight into the listings array and out the far side as
  // "No homes for sale", which is the exact class of confident claim this card
  // keeps having to be taught not to make.
  const bool isConfigured = (doc["isConfigured"] | true) && wire != WireStatus::NotConfigured;
  if (!isConfigured) {
    result.status = Status::NotConfigured;
    // A fixed sentence, not the server's. lastRefreshError on this path was
    // NotConfiguredResult's "RentCast API key is not configured. Sign up at
    // rentcast.io and set MyListings:ApiKey." - correct, actionable, and
    // addressed to whoever runs the deployment rather than to the household
    // this panel hangs in front of. It went to the log and the operator status
    // line instead; see Result::refreshError. A current server does not send it
    // to a device at all, which makes the leak impossible rather than merely
    // avoided - but this literal is what draws either way, so nothing here
    // depends on which server answered.
    result.message = "Real-estate listings are not set up for this home yet.";
    Log::printf("[listings] NOT CONFIGURED - resting; neither an empty market nor a failed "
                "refresh, and the listings array was not consulted");
    // A reason only when one was sent. Having none is the normal, correct state
    // on a stripped payload rather than a gap: the sentence lives on the
    // server's operator routes (/diag/providers, by-zip, for-user), which is
    // where the person who can act on it already reads it. Logged as its own
    // line so a 120-character reason cannot push the decision above it past
    // Log's 256-byte scratch buffer.
    if (hasRefreshError) {
      Log::printf("[listings] not-configured reason (operator text, never drawn): %s",
                  result.refreshError.c_str());
    } else {
      Log::verbose("[listings] no not-configured reason on the wire - expected on any current "
                   "server, which keeps operator diagnostics off device payloads");
    }
    return result;
  }

  JsonArrayConst listings = doc["listings"].as<JsonArrayConst>();
  if (listings.isNull() || listings.size() == 0) {
    // AN EMPTY LIST IS NOT ONE FACT, IT IS TWO, and they were being told apart
    // by nothing at all. When the refresh succeeded the server asked RentCast,
    // got an answer, and the answer was "nothing" - a real statement about the
    // market. When it failed the server never got an answer at all and is
    // serving the empty cache row FailAsync creates precisely so the reason is
    // recorded somewhere; the list is empty because the question failed, not
    // because the market is. Observed live on 2026-09-13 with a rejected
    // RentCast key: a device drew "No listings nearby right now" to a household
    // in a market with houses in it, and nothing anywhere said otherwise.
    //
    // `refreshFailed` above is what tells them apart now. It used to be
    // `hasRefreshError` - the presence of an operator's sentence - which is why
    // stripping that sentence from device payloads would have put this defect
    // straight back: every empty list would have looked like an answered
    // question. On a stripped payload this reads `status: "Unavailable"`
    // instead, which is the server saying the same thing on purpose.
    //
    // Deliberately NOT serviceUnreachable. That flag means "this device could
    // not reach OUR server", which is the thing a declared maintenance window
    // explains; this is our server answering perfectly well about a third party
    // it could not reach. A maintenance window and a refresh error are
    // different conditions and must not be relabelled as each other - so
    // failureText() passes this message through untouched, which is the right
    // outcome and the reason the flag stays false rather than an oversight.
    // None of the four ProviderStatus values means "the server is unreachable",
    // so nothing arriving in this field may ever raise that flag.
    if (refreshFailed) {
      result.status = Status::RefreshFailed;
      // Says what did not happen, and pointedly does not say what is or is not
      // for sale. No market is named as empty and no number is implied.
      result.message = "The listings" + describeMarket(result.cityState) +
                       " could not be refreshed just now.";
      Log::printf("[listings] empty list AND a failed refresh -> RefreshFailed; Empty NOT taken - "
                  "nothing here licenses a claim about the market. serviceUnreachable stays false: "
                  "our server answered, RentCast did not");
      if (hasRefreshError) {
        Log::printf("[listings] refresh failure reason (operator text, never drawn): %s",
                    result.refreshError.c_str());
      } else {
        Log::verbose("[listings] no failure reason on the wire - status alone said so, which is "
                     "the whole point of it being a status");
      }
      return result;
    }
    result.status = Status::Empty;
    result.message = "No homes for sale" + describeMarket(result.cityState) + " right now.";
    // The one branch on this card that makes a positive claim about somebody's
    // local property market, and until now the only one that logged nothing at
    // all. It says so now, and says what it is relying on to be allowed to: a
    // refresh the server reported as successful. If this line ever appears for
    // a market that plainly has houses in it, the signal above is what to
    // distrust.
    Log::printf("[listings] empty list and a SUCCESSFUL refresh -> Empty; RefreshFailed NOT taken "
                "- the server asked and the answer was 'nothing', so this is a real statement "
                "about %s and is drawn as one",
                result.cityState.length() > 0 ? result.cityState.c_str() : "the target market");
    return result;
  }

  // Listings AND a failed refresh - `status: "Stale"` on the new wire shape,
  // which is exactly the state that value was added to name. The server is
  // serving last-known-good rows while its refresh fails behind them
  // (RefreshCoreAsync keeps them on purpose). Drawing real listings beats
  // drawing a warning, and the card's own "Updated N min ago" line already
  // understates their age rather than overstating it, so the screen is left
  // alone and only the stream is told.
  if (refreshFailed) {
    Log::printf("[listings] serving %u cached listing(s) behind a failed refresh - drawing them "
                "rather than a warning, and NOT taking RefreshFailed: real rows beat a warning, "
                "and 'Updated N min ago' already understates their age",
                static_cast<unsigned>(listings.size()));
    if (hasRefreshError) {
      Log::printf("[listings] stale-rows reason (operator text, never drawn): %s",
                  result.refreshError.c_str());
    }
  } else {
    Log::verbose("[listings] %u listing(s) behind a refresh the server reported as successful - "
                 "nothing stale about these rows",
                 static_cast<unsigned>(listings.size()));
  }

  result.status = Status::Ok;
  result.count = 0;
  for (JsonVariantConst listing : listings) {
    if (result.count >= kMaxListings) {
      break;
    }
    ListingInfo& info = result.listings[result.count];
    info.address = String((const char*)(listing["address"] | ""));
    info.propertyType = String((const char*)(listing["propertyType"] | ""));
    info.price = listing["price"] | 0;
    info.bedrooms = listing["bedrooms"] | 0.0;
    info.bathrooms = listing["bathrooms"] | 0.0;
    info.squareFootage = listing["squareFootage"] | 0;
    info.daysOnMarket = listing["daysOnMarket"] | 0;
    info.distanceMiles = listing["distanceMiles"] | 0.0;
    info.mlsNumber = String((const char*)(listing["mlsNumber"] | ""));
    result.count++;
  }

  // A lightly-summarized response rather than the raw body - same filtering
  // reasoning as Forecast::fetch()'s/Aircraft::fetchMine()'s own verbose
  // lines: this endpoint's JsonDocument filter above already exists to keep
  // only the fields this card draws in memory.
  Log::verbose("[listings] response cityState=%s count=%u nearest='%s' $%d %.1fmi",
              result.cityState.c_str(), static_cast<unsigned>(result.count),
              result.listings[0].address.c_str(), result.listings[0].price,
              result.listings[0].distanceMiles);

  return result;
}

// ---------------------------------------------------------------------------
// The card descriptor. See the equivalent block at the bottom of
// Weather.cpp/Aircraft.cpp for why registration happens here rather than in
// App.ino.
//
// A genuine multi-item list card - see Listings.h's own remarks on how this
// differs from Aircraft's "list kind, one item shown" today. cardItemCount()
// reports the real count (capped at kMaxListings) so the scheduler cycles
// through the nearest few one at a time, each getting its own dwell, exactly
// the way it already cycles through any other list card's items.
// ---------------------------------------------------------------------------
namespace {

Result gLast;
bool gEverFetched = false;

/// millis() when gLast last became an Ok result - same field, same reasoning
/// as Weather.cpp's/Aircraft.cpp's gLastOkMs.
unsigned long gLastOkMs = 0;

/// Unsigned subtraction, correct across the millis() rollover at ~49 days -
/// identical to Weather.cpp's/Aircraft.cpp's describeFreshness(), duplicated
/// rather than shared for the same reason Aircraft.cpp's own copy is: the
/// three cards' Result types are unrelated and a shared helper would need a
/// fourth file just to hold one function used three times.
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

/// Re-asserted once per check-in - see Cards.h's StatusFn. NotConfigured is
/// called out distinctly from the refusals on purpose: "nobody has put a
/// RentCast key on file" is an operational resting state someone can act on,
/// and reading it as a failure sends whoever is watching after the wrong bug.
String cardStatus() {
  if (!gEverFetched) {
    return "never fetched";
  }
  switch (gLast.status) {
    case Status::Ok:
      return String("ok, ") + gLast.count + " listing(s)";
    case Status::Empty:
      return "ok, none listed nearby";
    // The one status line that carries the server's own words WHEN IT HAS THEM,
    // because this is the reader who can act on them - an admin looking at
    // /diag wants "401" or "budget exhausted", not the softened sentence the
    // card draws. Post-strip it has none: the words no longer reach a device at
    // all, and the admin reads them on the server's own operator routes
    // instead. Both cases already had a no-words form; RefreshFailed did not
    // and would have rendered a line ending in ": " with nothing after it.
    case Status::RefreshFailed:
      return gLast.refreshError.length() > 0
                 ? String("upstream refresh failed: ") + gLast.refreshError
                 : String("upstream refresh failed (reason is on the server, not the device)");
    case Status::NotConfigured:
      return gLast.refreshError.length() > 0
                 ? String("resting: not configured - ") + gLast.refreshError
                 : String("resting: no listings provider key on file");
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
    Log::printf("[listings] card updated: %u listing(s), nearest '%s' at %.1fmi",
                gLast.count, gLast.listings[0].address.c_str(), gLast.listings[0].distanceMiles);
  }
}

/// Real count while Ok (capped at kMaxListings by fetchMine() itself), one
/// item for any other status - same "a message is content too" tolerance
/// Weather's and Aircraft's own cardItemCount() already give their resting
/// and error states.
uint16_t cardItemCount() {
  if (!gEverFetched) {
    return 0;
  }
  return gLast.status == Status::Ok ? gLast.count : 1;
}

/// A listing new enough to be worth a longer look. 3 days mirrors the "just
/// listed" window a house hunter would actually care about, the same
/// scale-to-what-matters reasoning Aircraft.cpp's own isNotable() uses for
/// "nearly overhead" (a fraction of the tracking radius) rather than a fixed
/// distance.
bool cardIsNotable(uint16_t itemIndex) {
  if (gLast.status != Status::Ok || itemIndex >= gLast.count) {
    return false;
  }
  return gLast.listings[itemIndex].daysOnMarket <= 3;
}

void cardDraw(uint16_t itemIndex) {
  if (gLast.status == Status::Ok) {
    if (itemIndex >= gLast.count) {
      itemIndex = 0;
    }
    const ListingInfo& listing = gLast.listings[itemIndex];
    // Which of the (possibly several) fetched listings is on screen this
    // draw, not just what the last fetch found - this card, unlike
    // Aircraft's and Forecast's, genuinely pages through multiple items, so
    // "item 2/5" alone (CardManager's own choke-point line) does not say
    // which address that actually is.
    Log::verbose("[listings] drawing item %u/%u: '%s' $%d %.1fmi",
                static_cast<unsigned>(itemIndex) + 1, static_cast<unsigned>(gLast.count),
                listing.address.c_str(), listing.price, listing.distanceMiles);
    Display::showListingsCard(listing.address, listing.propertyType, listing.price,
                              listing.bedrooms, listing.bathrooms, listing.squareFootage,
                              listing.daysOnMarket, listing.distanceMiles,
                              /*index=*/itemIndex, /*total=*/gLast.count,
                              describeFreshness(gLastOkMs));
    return;
  }

  // Empty and NotConfigured are resting states - nothing wrong with the
  // device, just nothing to show or nothing set up yet - shown muted rather
  // than amber, the same isProblem split Weather's and Aircraft's cards make.
  //
  // RefreshFailed rests too, and that is a judgement rather than an oversight.
  // Amber says "something is wrong with this device", and nothing is: the panel
  // is fine, the network is fine, our server answered. What failed is a
  // third-party feed that only an administrator can restore, exactly like
  // ProviderDisabled and NotConfigured which are already muted. Amber in a
  // kitchen for a fault nobody in that kitchen can fix is an alarm that trains
  // its reader to ignore alarms. The urgency belongs in cardStatus() and the
  // debug stream, where somebody can act on it.
  const bool isRestingState = gLast.status == Status::NotActivated ||
                              gLast.status == Status::ProviderDisabled ||
                              gLast.status == Status::NotConfigured ||
                              gLast.status == Status::RefreshFailed ||
                              gLast.status == Status::Empty;
  const String headline = gLast.status == Status::Empty  ? "No listings nearby right now"
                          : gLast.status == Status::RefreshFailed ? "Could not check for listings"
                          : gLast.status == Status::NotConfigured ? "Listings are not set up yet"
                          : isRestingState                        ? "Listings are not showing yet"
                                                                  : "Could not load listings";
  // Resolved on this draw rather than at fetch time - see Forecast.cpp's
  // identical call site and Maintenance.h. The log line keeps the raw message on
  // purpose; the debug stream wants the fault, not the reassurance.
  const String detail = Maintenance::failureText(gLast.message, gLast.serviceUnreachable);
  Log::verbose("[listings] drawing status screen: %s (%s)", headline.c_str(),
              gLast.message.c_str());
  Display::showListingsStatus(headline, detail, /*isProblem=*/!isRestingState);
}

/// The line a press carries: which house was on screen when somebody tapped
/// Interested. This is the card the whole mechanism exists for - a press here is
/// about one specific listing, and without this the server is told only that the
/// listings card was pressed. See Cards::DescribeFn.
///
/// Address and price lead, because those identify the property to whoever reads
/// the email. Beds and baths follow, because an agent scanning a list of
/// enquiries uses them to tell two similar addresses apart. Distance and
/// days-on-market are deliberately left out: both are relative to this device
/// and this moment, and neither survives usefully into an email read hours later.
///
/// A status screen returns empty rather than "No listings nearby". A press on a
/// card showing nothing has nothing to name, and inventing a description would
/// put a sentence in the press log that reads like content.
String cardDescribe(uint16_t itemIndex) {
  if (gLast.status != Status::Ok || itemIndex >= gLast.count) {
    return String();
  }

  const ListingInfo& listing = gLast.listings[itemIndex];
  String summary = listing.address;
  if (listing.price > 0) {
    summary += " - $" + String(listing.price);
  }
  if (listing.bedrooms > 0 || listing.bathrooms > 0) {
    summary += " - " + String(listing.bedrooms, 0) + "bd/" + String(listing.bathrooms, 1) + "ba";
  }
  // The MLS number appears here and nowhere else on this device: never on the
  // card, only in what a press carries. It is last because it is the one part
  // written for the recipient rather than for the household - an agent reading
  // the email can look the listing up by it, and if the 200-character cap ever
  // truncates this string it is the right thing to lose, since the address
  // above already identifies the property.
  if (listing.mlsNumber.length() > 0) {
    summary += " - MLS " + listing.mlsNumber;
  }
  return summary;
}

[[maybe_unused]] const bool kRegistered = [] {
  Cards::CardSpec spec;
  spec.id = "listings";
  spec.kind = Cards::Kind::List;
  spec.fetch = cardFetch;
  spec.itemCount = cardItemCount;
  spec.describe = cardDescribe;
  spec.draw = cardDraw;
  spec.isNotable = cardIsNotable;
  spec.status = cardStatus;
  spec.order = 3;
  spec.dwellSeconds = 10;
  spec.notableDwellSeconds = 18;
  return Cards::registerCard(spec);
}();

}  // namespace

}  // namespace Listings
