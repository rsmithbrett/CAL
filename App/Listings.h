#pragma once

#include <Arduino.h>

/// Real-estate listings near the device owner's Target market - the third
/// server-fetched content type, sitting beside Weather.h/Aircraft.h as its
/// own module per Weather.h's own remarks on why cards get sibling files
/// rather than growing into one another. Like Weather and Aircraft, the .cpp
/// registers its own card descriptor with the scheduler (see Cards.h) rather
/// than being named anywhere above it.
///
/// Unlike Weather and Aircraft, this registers as a *list* card in the literal
/// sense: MyListingsService returns several distance-sorted listings, not one
/// reading to feature, and this module keeps up to kMaxListings of them and
/// lets the scheduler cycle through them one per dwell - see Aircraft.h's own
/// remarks on why *it* is a list card showing only one item today, and note
/// that this module is exactly the "later server change that hands over the
/// whole list" scenario that comment describes: cardItemCount() here reports
/// the real count and cardDraw() reads the index it is given, no scheduler
/// change required either way.
namespace Listings {

enum class Status {
  Ok,
  Empty,             // the fetch succeeded; nothing is listed nearby right now - not an error
  NotConfigured,     // ListingsResult.IsConfigured == false (no RentCast key on file) - an
                     // operational resting state, not a device problem - see ListingsResult
                     // on the server for why this is a first-class value rather than an
                     // error string to pattern-match.
  NotActivated,      // ContentGateRefusal.DeviceNotActivated
  ProviderDisabled,  // ContentGateRefusal.ProviderDisabled
  AuthError,         // the device's own secret was rejected
  NetworkError,      // couldn't reach the service, or the response made no sense
};

/// How many of the server's (already distance-sorted) listings this card
/// keeps and cycles through. The server can return more than this for a
/// dense market; this is a display cap, not a request parameter - there is
/// no "how many to send" knob on GET /api/mylistings/mine, the same as
/// Aircraft has no such knob on its own endpoint. 5 mirrors the task's own
/// "a small number, nearest first" framing: enough to give a real sense of
/// what's on the market without turning the rotation into an open-ended
/// scroll through a single card type.
static constexpr uint8_t kMaxListings = 5;

/// One listing, trimmed to what Display::showListingsCard() actually draws -
/// mirrors ListingSummary on the server (DiscoverAroundMe.Providers.Data).
struct ListingInfo {
  String address;
  String propertyType;
  int price = 0;
  double bedrooms = 0;
  double bathrooms = 0;
  int squareFootage = 0;
  int daysOnMarket = 0;
  double distanceMiles = 0;
};

struct Result {
  Status status = Status::NetworkError;
  uint8_t count = 0;
  ListingInfo listings[kMaxListings];
  /// The Target market's city/state, when the server sent one - drawn as a
  /// caption on the Empty/NotConfigured screens ("No homes for sale near
  /// Charlotte, NC right now") the same way Weather's location line prefers
  /// a city name over a bare postal code. Empty when the server didn't send
  /// one, in which case those screens fall back to a market-less phrasing.
  String cityState;
  /// Set on every non-Ok status, including Empty - what to put on screen.
  String message;
};

/// GETs /api/mylistings/mine with the device's own secret and no id anywhere
/// in the request - identical authentication to Weather::fetchMine() and
/// Aircraft::fetchMine() (see MyListingsEndpoints.cs's remarks, which mirror
/// MyWeatherEndpoints.cs's own for why "mine" replaced an id-bearing route).
Result fetchMine();

}  // namespace Listings
