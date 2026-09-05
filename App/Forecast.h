#pragma once

#include <Arduino.h>

/// The multi-day weather outlook card ("forecast" - see KnownCardCatalog on
/// the server, which registers it with UsesLocationChoice: true). Sits beside
/// Weather.h/Listings.h as its own module for the same reason every other
/// server-fetched content type does (see Weather.h's own remarks on why
/// cards get sibling files rather than growing into one another).
///
/// This is the first card on this build that reads a per-card Location
/// choice off its own policy row rather than the server deciding Home-vs-
/// Target on its own the way Weather's "mine" endpoint does. See
/// Cards::CardSpec::location for how that field arrives on the wire and
/// CardManager::applyPolicy() for how it lands on this card's own
/// descriptor; Forecast.cpp's wantsTarget() is what turns it into the
/// "home"/"target" query value GET /api/myweather/forecast expects.
///
/// A genuine list card, like Listings: the server returns several periods
/// (up to kMaxPeriods), oldest/nearest first, and the scheduler cycles
/// through them one per dwell exactly the way it already does for listings -
/// see Listings.h's own remarks on why that differs from Aircraft's "list
/// kind, one item shown" today.
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

/// Matches MyWeatherEndpoints.MaxForecastPeriods (8) on the server, which is
/// what actually bounds how many periods a response ever carries - this only
/// sizes the fixed array those periods are kept in once they arrive.
static constexpr uint8_t kMaxPeriods = 8;

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
  /// to a bare postal code the same way Weather.cpp's own describeLocation()
  /// does - empty when the location could not be resolved at all.
  String location;
  /// Set on every non-Ok status, including Empty - what to put on screen.
  /// Empty on Ok, since the card itself is the message.
  String message;
};

/// GETs /api/myweather/forecast?location=home|target with the device's own
/// secret and no id anywhere in the request - identical authentication to
/// Weather::fetchMine()/Listings::fetchMine(). `useTarget` selects which of
/// the two query values is sent; the card's own cardFetch() resolves this
/// from this card's own descriptor Location field (see wantsTarget() in
/// Forecast.cpp) before calling this, so this function itself stays a plain,
/// parameterised fetch with no policy-reading of its own.
Result fetch(bool useTarget);

}  // namespace Forecast
