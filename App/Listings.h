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
  Empty,             // the fetch succeeded, the server's own upstream refresh succeeded, and
                     // nothing is listed nearby right now - not an error. This is the ONLY
                     // status that licenses a claim about the market, which is why
                     // RefreshFailed below had to be split out of it.
  RefreshFailed,     // the fetch succeeded and the list came back empty, but the server also
                     // sent a lastRefreshError - so the emptiness is the absence of an
                     // answer, not an answer. See the field of that name on Result, and
                     // fetchMine()'s own remarks on why "nothing nearby" and "we could not
                     // find out" must not share a status.
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

/// One listing, trimmed to what Display::showListingsCard() draws plus the one
/// field it deliberately does not - mirrors ListingSummary on the server
/// (DiscoverAroundMe.Providers.Data).
struct ListingInfo {
  String address;
  String propertyType;
  int price = 0;
  double bedrooms = 0;
  double bathrooms = 0;
  int squareFootage = 0;
  int daysOnMarket = 0;
  double distanceMiles = 0;

  /// The MLS number, when the server has one. Empty is ordinary - not every
  /// sale listing is an MLS listing, and RentCast's coverage varies by market.
  ///
  /// **Held but never drawn**, which makes it the only field here that
  /// showListingsCard() ignores. It is carried so a button press can take it
  /// into the press log and the notification email, where the recipient is an
  /// agent who can look the listing up. On a card read from across a room it
  /// would be eight characters of noise beside the address, which is what
  /// identifies the house to the household actually looking at it. See
  /// cardDescribe() in Listings.cpp and Cards::DescribeFn.
  ///
  /// A deliberate exception to the rule the fetch filter otherwise follows -
  /// "only the fields this card actually draws are worth keeping" - so do not
  /// remove it as unused. Roughly 10 bytes per listing, kMaxListings of them.
  String mlsNumber;
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
  ///
  /// Always the plain, honest text, even during a declared maintenance window -
  /// the window is applied on the way to the screen by
  /// Maintenance::failureText() at draw time, never baked in here. Identical
  /// contract, for identical reasons, to Forecast::Result::message; see that
  /// field's own remarks and Maintenance.h.
  ///
  /// **Never carries text the server generated.** Every value assigned to it is
  /// a literal in Listings.cpp, chosen for a household reading a wall display
  /// from across a room. The server's own words go in `refreshError` below and
  /// reach the debug stream, never the panel. This used to be violated on the
  /// NotConfigured path, which put ListingsResult's "Sign up at rentcast.io and
  /// set MyListings:ApiKey" straight onto somebody's kitchen wall.
  String message;

  /// The server's `lastRefreshError`, when it sent one - why ITS most recent
  /// attempt to refresh from RentCast did not produce fresh data. Populated on
  /// any 200 that carries the field, whatever the resulting status: a rejected
  /// key, an exhausted monthly budget, an unresolvable postal code, or a bare
  /// exception message.
  ///
  /// **For the debug stream and the operator status line only. Never drawn.**
  /// It is written for whoever administers the deployment - it names API
  /// dashboards, config keys and upstream vendors - and none of that is for the
  /// household whose wall this is on. The card says the listings could not be
  /// refreshed; this says why, to someone who can act on it.
  ///
  /// Truncated to a length worth keeping in RAM (the column is 1000 characters
  /// server-side, and this lives in the retained gLast for as long as the state
  /// does). The head of the string is the part that identifies the fault.
  String refreshError;

  /// Whether this failure is one the words "cannot reach the service" were being
  /// used for. True at exactly the three sites that set `message` to "Cannot
  /// reach the listings service." and nowhere else - notably NOT at the TLS-setup
  /// failure, which is a fault on this device's own side of the wire that a
  /// planned server outage does not explain. Identical contract, for identical
  /// reasons, to Forecast::Result::serviceUnreachable; see that field's own
  /// remarks. Read only by Maintenance::failureText(); when no window is in force
  /// it changes nothing at all.
  bool serviceUnreachable = false;
};

/// GETs /api/mylistings/mine with the device's own secret and no id anywhere
/// in the request - identical authentication to Forecast::fetch() and
/// Aircraft::fetchMine() (see MyListingsEndpoints.cs's remarks, which mirror
/// MyWeatherEndpoints.cs's own for why "mine" replaced an id-bearing route).
Result fetchMine();

}  // namespace Listings
