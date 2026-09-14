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
  // available. isConfigured/lastRefreshError are top-level siblings of the
  // array, not part of it, so they get their own filter entries.
  // Built once and reused for the life of the device - see Aircraft.cpp's own
  // filter for the full reasoning. Short version: a JsonDocument takes a 1KB
  // heap pool block the moment it holds anything, this one's contents never
  // vary, and re-taking that block on every fetch was pure churn on a device
  // whose scarce resource is contiguous blocks.
  static const JsonDocument filter = [] {
    JsonDocument f;
    f["city"] = true;
    f["state"] = true;
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

  // Read once, up front, because every branch below wants to know about it and
  // the difference it makes to each is different. The server sets it whenever
  // ITS own refresh from RentCast did not produce fresh data - a rejected key,
  // an exhausted monthly budget, an unresolvable postal code, a bare exception
  // message (MyListingsService.FailAsync). It has been on the wire since this
  // endpoint existed and this filter has whitelisted it since this card was
  // written; nothing read it except the NotConfigured branch, which is how a
  // rejected key came to render as a confident claim about the market.
  const char* lastError = doc["lastRefreshError"] | "";
  const bool hasRefreshError = strlen(lastError) > 0;
  if (hasRefreshError) {
    result.refreshError = retainRefreshError(lastError);
  }

  // A first-class resting state, not an error - see ListingsResult on the
  // server. Checked before ever looking at the listings array: an
  // unconfigured account still gets served whatever stale listings happen to
  // be cached, and this card should read that the same way the server itself
  // treats it - as "not set up yet", not as "nothing nearby".
  const bool isConfigured = doc["isConfigured"] | true;
  if (!isConfigured) {
    result.status = Status::NotConfigured;
    // A fixed sentence, not the server's. lastRefreshError on this path is
    // NotConfiguredResult's "RentCast API key is not configured. Sign up at
    // rentcast.io and set MyListings:ApiKey." - correct, actionable, and
    // addressed to whoever runs the deployment rather than to the household
    // this panel hangs in front of. It goes to the log and the operator status
    // line instead; see Result::refreshError.
    result.message = "Real-estate listings are not set up for this home yet.";
    Log::printf("[listings] not configured: %s",
                hasRefreshError ? result.refreshError.c_str() : "(no reason given)");
    return result;
  }

  JsonArrayConst listings = doc["listings"].as<JsonArrayConst>();
  if (listings.isNull() || listings.size() == 0) {
    // AN EMPTY LIST IS NOT ONE FACT, IT IS TWO, and they were being told apart
    // by nothing at all. With no refresh error the server asked RentCast, got
    // an answer, and the answer was "nothing" - a real statement about the
    // market. With a refresh error the server never got an answer at all and is
    // serving the empty cache row FailAsync creates precisely so the reason is
    // recorded somewhere; the list is empty because the question failed, not
    // because the market is. Observed live on 2026-09-13 with a rejected
    // RentCast key: a device drew "No listings nearby right now" to a household
    // in a market with houses in it, and nothing anywhere said otherwise.
    //
    // Deliberately NOT serviceUnreachable. That flag means "this device could
    // not reach OUR server", which is the thing a declared maintenance window
    // explains; this is our server answering perfectly well about a third party
    // it could not reach. A maintenance window and a refresh error are
    // different conditions and must not be relabelled as each other - so
    // failureText() passes this message through untouched, which is the right
    // outcome and the reason the flag stays false rather than an oversight.
    if (hasRefreshError) {
      result.status = Status::RefreshFailed;
      // Says what did not happen, and pointedly does not say what is or is not
      // for sale. No market is named as empty and no number is implied.
      result.message = "The listings" + describeMarket(result.cityState) +
                       " could not be refreshed just now.";
      Log::printf("[listings] empty list WITH a refresh error - reporting a failed refresh, not an "
                  "empty market: %s",
                  result.refreshError.c_str());
      return result;
    }
    result.status = Status::Empty;
    result.message = "No homes for sale" + describeMarket(result.cityState) + " right now.";
    return result;
  }

  // Listings AND an error: the server is serving last-known-good rows while its
  // refresh fails behind them (RefreshCoreAsync keeps them on purpose). Drawing
  // real listings beats drawing a warning, and the card's own "Updated N min
  // ago" line already understates their age rather than overstating it, so the
  // screen is left alone and only the stream is told.
  if (hasRefreshError) {
    Log::printf("[listings] serving %u cached listing(s) behind a failed refresh: %s",
                static_cast<unsigned>(listings.size()), result.refreshError.c_str());
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
    // The one status line that carries the server's own words, because this is
    // the reader who can act on them - an admin looking at /diag needs to see
    // "401" or "budget exhausted", not the softened sentence the card draws.
    case Status::RefreshFailed:
      return String("upstream refresh failed: ") + gLast.refreshError;
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
