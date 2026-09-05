#include "Listings.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>

#include "Cards.h"
#include "Config.h"
#include "Display.h"
#include "Identity.h"
#include "Log.h"
#include "Tls.h"

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

}  // namespace

Result fetchMine() {
  Result result;

  NetworkClientSecure client;
  if (!Tls::configure(client)) {
    result.message = "Cannot verify the service's identity.";
    Log::line("[listings] TLS setup failed");
    return result;
  }

  HTTPClient http;
  const String url = String("https://") + Config::kServiceHost + kPath;
  if (!http.begin(client, url)) {
    result.message = "Cannot reach the listings service.";
    Log::line("[listings] could not begin request");
    return result;
  }
  http.setTimeout(Config::kHttpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());

  const int status = http.GET();

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
  JsonDocument filter;
  filter["city"] = true;
  filter["state"] = true;
  filter["isConfigured"] = true;
  filter["lastRefreshError"] = true;
  filter["listings"][0]["address"] = true;
  filter["listings"][0]["propertyType"] = true;
  filter["listings"][0]["price"] = true;
  filter["listings"][0]["bedrooms"] = true;
  filter["listings"][0]["bathrooms"] = true;
  filter["listings"][0]["squareFootage"] = true;
  filter["listings"][0]["daysOnMarket"] = true;
  filter["listings"][0]["distanceMiles"] = true;

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

  // A first-class resting state, not an error - see ListingsResult on the
  // server. Checked before ever looking at the listings array: an
  // unconfigured account still gets served whatever stale listings happen to
  // be cached, and this card should read that the same way the server itself
  // treats it - as "not set up yet", not as "nothing nearby".
  const bool isConfigured = doc["isConfigured"] | true;
  if (!isConfigured) {
    result.status = Status::NotConfigured;
    const char* lastError = doc["lastRefreshError"] | "";
    result.message = strlen(lastError) > 0
        ? String(lastError)
        : "Real-estate listings are not configured for this account yet.";
    Log::printf("[listings] not configured: %s", result.message.c_str());
    return result;
  }

  JsonArrayConst listings = doc["listings"].as<JsonArrayConst>();
  if (listings.isNull() || listings.size() == 0) {
    result.status = Status::Empty;
    result.message = "No homes for sale" + describeMarket(result.cityState) + " right now.";
    return result;
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
    result.count++;
  }

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
  const bool isRestingState = gLast.status == Status::NotActivated ||
                              gLast.status == Status::ProviderDisabled ||
                              gLast.status == Status::NotConfigured ||
                              gLast.status == Status::Empty;
  const String headline = gLast.status == Status::Empty  ? "No listings nearby right now"
                          : gLast.status == Status::NotConfigured ? "Listings are not set up yet"
                          : isRestingState                        ? "Listings are not showing yet"
                                                                  : "Could not load listings";
  Display::showListingsStatus(headline, gLast.message, /*isProblem=*/!isRestingState);
}

[[maybe_unused]] const bool kRegistered = [] {
  Cards::CardSpec spec;
  spec.id = "listings";
  spec.kind = Cards::Kind::List;
  spec.fetch = cardFetch;
  spec.itemCount = cardItemCount;
  spec.draw = cardDraw;
  spec.isNotable = cardIsNotable;
  spec.order = 3;
  spec.dwellSeconds = 10;
  spec.notableDwellSeconds = 18;
  return Cards::registerCard(spec);
}();

}  // namespace

}  // namespace Listings
