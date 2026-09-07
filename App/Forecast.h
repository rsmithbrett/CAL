#pragma once

#include <Arduino.h>

/// The current-conditions-and-outlook card ("forecast" - see
/// KnownCardCatalog on the server, which registers it with
/// UsesLocationChoice: true). Sits beside Listings.h as its own module for
/// the same reason every other server-fetched content type does (see
/// Listings.h's own remarks on why cards get sibling files rather than
/// growing into one another). It also absorbed the standalone weather
/// card's old role - see the README's "The weather card retired, folded
/// into Forecast" for that history; there is no Weather.h any more.
///
/// This is the first card on this build that reads a per-card Location
/// choice off its own policy row rather than the server deciding Home-vs-
/// Target for it. See Cards::CardSpec::location for how that field arrives
/// on the wire and CardManager::applyPolicy() for how it lands on this
/// card's own descriptor; Forecast.cpp's wantsTarget() is what turns it into
/// the "home"/"target" query value GET /api/myweather/forecast expects.
///
/// The server returns several periods (up to kMaxPeriods), oldest/nearest
/// first, but unlike Listings this card no longer pages through them one per
/// dwell - it draws every one it has in a single combined slide, periods[0]
/// as a "right now" hero and periods[0, 2, 4, ...] as a five-day strip
/// beneath it. See Display::showForecastCard()'s own remarks for the layout
/// and Forecast.cpp's Instance<N>::draw() for how the strip is built.
///
/// **This module provides five independently-configured instances**, ids
/// `"forecast"` through `"forecast5"`, the same Instance<N> template idiom
/// Graphic.cpp established - the one fetch-driven card in this build that
/// gets that treatment. The judgment call, recorded here because Brett asked
/// for it explicitly: Weather, Aircraft, Listings and Tides each hit a
/// "mine"-style endpoint (or, for Tides, unconditional check-in data) with
/// **no policy-configurable parameter at all** - a second instance of any of
/// those would issue an identical request and draw an identical card, for no
/// benefit and an extra HTTP round trip every refresh. Forecast is
/// different: it already reads a per-card Location choice
/// (`Cards::CardSpec::location`), so a second instance is a real,
/// independently-useful configuration - "home forecast" and "target
/// forecast" as two separate rotation entries - not a redundant duplicate.
/// That is the dividing line this generalisation drew: a card gets multiple
/// instances when its own descriptor already carries a field an admin can
/// set differently per instance (assetId for Graphic, text for Announcement,
/// qrData for QrText, location for Forecast), not merely because it has no
/// network fetch. See Cards.h's own kMaxCards remarks for the fuller
/// accounting of which of the eleven other card types did and did not get
/// this treatment, and why.
///
/// Each instance independently fetches: instance 2 configured for "target"
/// does its own GET /api/myweather/forecast?location=target on its own
/// refresh timer, entirely unaware of what instance 1 fetched or when. That
/// is more network traffic than a single forecast card generates - bounded,
/// once a real cardPolicy has arrived, to only the instances an admin
/// actually named (CardManager::applyPolicy() deactivates every registered
/// card not mentioned in the policy, and an inactive card's `fetch` is never
/// called - see CardManager.cpp's `refreshOneDueCard()`). Before that first
/// check-in, though, every `CardSpec::active` defaults to true (see Cards.h),
/// so a freshly-booted device fetches all five instances once each - one
/// unconditional GET per instance, exactly as the original lone forecast
/// card always has - before the first policy narrows the set down. That
/// transient is bounded (five requests, once, at boot) and self-corrects the
/// moment check-in completes; it was true of the single-instance card too,
/// just multiplied by five now, and is called out here rather than left for
/// someone reading a cold-boot log to puzzle out on their own.
namespace Forecast {

enum class Status {
  Ok,
  // The fetch succeeded but there is nothing to draw: either the requested
  // location could not be resolved at all (GET /api/myweather/forecast still
  // answers 200 with city/state/postalCode/periods all empty rather than a
  // top-level null - see MyWeatherEndpoints.ForDeviceForecast's own remarks),
  // or it resolved but genuinely has no periods. Both collapse to the same
  // "report nothing to draw" outcome this card gives the scheduler - there is
  // no richer distinction worth showing a household that the ordinary
  // "no forecast available" message doesn't already cover.
  Empty,
  NotActivated,      // ContentGateRefusal.DeviceNotActivated
  ProviderDisabled,  // ContentGateRefusal.ProviderDisabled
  AuthError,         // the device's own secret was rejected
  NetworkError,      // couldn't reach the service, or the response made no sense
};

/// Matches MyWeatherEndpoints.MaxForecastPeriods (10) on the server, which is
/// what actually bounds how many periods a response ever carries - this only
/// sizes the fixed array those periods are kept in once they arrive. 10
/// periods is five day/night pairs - a real 5-day forecast, including today -
/// raised from 8/four days; see that constant's own remarks for the byte
/// budget this stays inside.
static constexpr uint8_t kMaxPeriods = 10;

/// One forecast period, trimmed to what Display::showForecastCard() actually
/// draws - mirrors the anonymous period shape
/// MyWeatherEndpoints.ForDeviceForecast sends (Name/IsDaytime/Temperature/
/// TemperatureUnit/ShortForecast; DetailedForecast never leaves the server at
/// all - see that function's own remarks on the incident that is why).
struct PeriodInfo {
  String name;
  bool isDaytime = true;
  int temperature = 0;
  String unit = "F";
  String shortForecast;
};

struct Result {
  Status status = Status::NetworkError;
  uint8_t count = 0;
  PeriodInfo periods[kMaxPeriods];
  /// City/state the server resolved the requested location to, falling back
  /// to a bare postal code the same way Forecast.cpp's own describeLocation()
  /// does - empty when the location could not be resolved at all.
  String location;
  /// Set on every non-Ok status, including Empty - what to put on screen.
  /// Empty on Ok, since the card itself is the message.
  String message;
};

/// GETs /api/myweather/forecast?location=home|target with the device's own
/// secret and no id anywhere in the request - identical authentication to
/// Listings::fetchMine(). `useTarget` selects which of
/// the two query values is sent; the card's own cardFetch() resolves this
/// from this card's own descriptor Location field (see wantsTarget() in
/// Forecast.cpp) before calling this, so this function itself stays a plain,
/// parameterised fetch with no policy-reading of its own.
Result fetch(bool useTarget);

/// This instance's registered id, and the `id` a policy entry must use to
/// schedule it or give it a Location choice. Exposed only so the id appears
/// exactly once in the firmware, the same convention Graphic::kCardId/
/// Announcement::kCardId/QrText::kCardId keep. Unlike those three, `fetch()`
/// above stays a free, parameterised function rather than something each
/// instance calls with no arguments - see Forecast.cpp's Instance<N>::fetch()
/// for why it is called fully qualified as `Forecast::fetch(useTarget)` from
/// inside each instantiation.
extern const char* const kCardId;

/// The second through fifth instances' registered ids, independent of
/// `kCardId` and of each other in every respect - own retained fetch state,
/// own Location choice, own place in the rotation. See Graphic::kCardId2's
/// identical wording.
extern const char* const kCardId2;
extern const char* const kCardId3;
extern const char* const kCardId4;
extern const char* const kCardId5;

}  // namespace Forecast
