#pragma once

#include <Arduino.h>

/// The one content type this first build of the App renders. Additional cards
/// (Listings.h/.cpp, Aircraft.h/.cpp, ...) are meant to sit beside this file as
/// their own modules mirroring the server's per-provider project split - see
/// MyWeatherEndpoints.cs / MyListingsEndpoints.cs / MyAircraftEndpoints.cs - not
/// to be folded into it.
namespace Weather {

enum class Status {
  Ok,
  NotActivated,      // ContentGateRefusal.DeviceNotActivated
  ProviderDisabled,  // ContentGateRefusal.ProviderDisabled
  AuthError,         // the device's own secret was rejected
  NetworkError,      // couldn't reach the service, or the response made no sense
};

struct Forecast {
  String location;
  int temperature = 0;
  String unit = "F";
  String shortForecast;
};

struct Result {
  Status status = Status::NetworkError;
  Forecast forecast;
  /// Set on every non-Ok status - what to put on screen. Empty on Ok, since
  /// the card itself is the message.
  String message;
};

/// GETs /api/myweather/mine with the device's own secret and no id anywhere in
/// the request (see MyWeatherEndpoints.cs's remarks on why) - identical
/// authentication to every other request the App and CAL both make.
Result fetchMine();

}  // namespace Weather
