#pragma once

#include <Arduino.h>
#include <time.h>  // time_t, for Result::fetchedAtUtc below.

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
  RefreshFailed,     // the fetch succeeded and the list came back empty, but the server's own
                     // upstream refresh did not - so the emptiness is the absence of an
                     // answer, not an answer. Read from the server's `status`
                     // (ProviderStatus.Unavailable), falling back on a pre-strip payload to
                     // the presence of `lastRefreshError`; see fetchMine()'s own remarks on
                     // both wire shapes, and on why "nothing nearby" and "we could not find
                     // out" must not share a status.
  NotConfigured,     // ListingsResult.IsConfigured == false, equivalently
                     // ProviderStatus.NotConfigured (no RentCast key on file) - an
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

  /// **When the SERVER last read RentCast**, in epoch seconds, or 0 when the
  /// payload carried no usable `fetchedAtUtc`. Not when this device asked.
  ///
  /// This is the only card that keeps this, and CARD_ABSENCE_AND_AGE_DESIGN.md
  /// section 2a is the whole argument for why it is one card and not all of
  /// them: the gap between "when I asked" and "how old the answer is" is
  /// bounded by the server's cache, weather and calendar cache for 30 minutes
  /// where the device's own fetch time is a fine proxy, and listings caches for
  /// 24 hours where it is not. On 2026-09-27 a device drew "Updated just now"
  /// over an answer a day and a half old.
  ///
  /// 0 is a real and expected value, not a fault: a server that somehow omits
  /// the field, a null, or anything the parser cannot read. The card falls back
  /// to the old device-side measurement in that case rather than drawing
  /// nothing or, worse, an age computed from a zero.
  time_t fetchedAtUtc = 0;

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
  /// from across a room. This used to be violated on the NotConfigured path,
  /// which put ListingsResult's "Sign up at rentcast.io and set
  /// MyListings:ApiKey" straight onto somebody's kitchen wall.
  String message;

  // NO FIELD ON THIS RESULT CARRIES THE SERVER'S WORDS, to any surface. There
  // was one - `refreshError`, a 120-character copy of `lastRefreshError` kept
  // here for the debug stream and the /diag status line - and it is gone.
  //
  // Not because that reader was wrong to want it. An admin at /diag does want
  // "401" or "budget exhausted". It is that the sentence is an operator
  // diagnostic and was never this device's to hold: the server marks
  // `LastRefreshError` `[OperatorDiagnostic]`, `DeviceJsonResult` strips every
  // marked property from device-facing payloads, and /diag/providers serves the
  // untruncated original to that same admin from the record it was written on.
  // Keeping a truncated copy in a retained Result, for as long as the failure
  // lasted, spent contiguous heap on a worse version of a surface that already
  // existed - and on the wire shape this fleet is moving to, spent it on a
  // field that is never sent.
  //
  // What the device still needs from `lastRefreshError` is not its text but
  // whether it arrived at all, and only on the pre-strip wire shape: see
  // fetchMine()'s `hasRefreshError`, which is a bool, is read once, and is
  // retained nowhere. The state this card draws comes from `status`
  // (ProviderStatus) instead.

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
