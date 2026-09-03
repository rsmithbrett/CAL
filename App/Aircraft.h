#pragma once

#include <Arduino.h>

/// Live aircraft overhead - the second content type this build renders,
/// sitting beside Weather.h as its own module per Weather.h's own remarks on
/// why cards get sibling files rather than growing into one another. Like
/// Weather, the .cpp registers its own card descriptor with the scheduler
/// (see Cards.h) rather than being named anywhere above it. It registers as
/// a *list* card - see the block at the bottom of Aircraft.cpp for why that
/// is the honest description even while the server hands over only the
/// nearest sighting.
///
/// CYD-Dickey's Aircraft.cpp/Aircraft.h talk to api.adsb.lol directly from
/// the device and additionally resolve, per callsign, an airline name/logo
/// (Airlines.h, user-managed on LittleFS) and a departure/arrival route
/// (Route.h, via hexdb.io). None of that richer data exists on this path:
/// DiscoverAroundMe's server-side MyAircraftService/AdsbLolClient (see
/// MyAircraftEndpoints.cs's "/mine" route) exposes exactly five fields per
/// aircraft - callsign, altitude, speed, heading, distance - already
/// filtered to airborne traffic within the owner's configured radius, with
/// no airline or route lookup anywhere in that pipeline. CYD-Dickey's
/// aircraft card's airline-name/logo panel and route line therefore have no
/// equivalent here; this module and Display::showAircraftCard() work with
/// only the five fields the server actually provides. Enriching the server
/// side with an airline/route lookup would be a separate, larger project,
/// not something this firmware can paper over on its own.
namespace Aircraft {

enum class Status {
  Ok,
  Empty,             // the fetch succeeded; nothing is within radius right now - not an error
  NotActivated,      // ContentGateRefusal.DeviceNotActivated
  ProviderDisabled,  // ContentGateRefusal.ProviderDisabled
  AuthError,         // the device's own secret was rejected
  NetworkError,      // couldn't reach the service, or the response made no sense
};

/// One aircraft, trimmed to what Display::showAircraftCard() actually draws -
/// mirrors AircraftSighting on the server (DiscoverAroundMe.Providers.Data).
struct Sighting {
  String callsign;
  int altitudeFeet = 0;
  double speedKnots = 0;
  double headingDegrees = 0;
  double distanceMiles = 0;
};

struct Result {
  Status status = Status::NetworkError;
  /// The nearest aircraft in range - valid only when status == Ok. The
  /// server already returns its Aircraft list sorted by distance ascending
  /// (MyAircraftService.RefreshCoreAsync's OrderBy), so this is simply
  /// element 0; CAL shows one featured aircraft per card the same way it
  /// shows one featured forecast period, rather than cycling through the
  /// full list CYD-Dickey's touch-driven card carousel does (this board has
  /// no touch input wired up at all - see App.ino).
  Sighting nearest;
  /// The owner's configured tracking radius, echoed back by the server on
  /// every response. Kept because "how interesting is this sighting" only
  /// means anything relative to it - a plane 2 miles out is practically
  /// overhead when the radius is 10 and unremarkable when it is 30. Same
  /// reasoning as CYD-Dickey scaling its own overhead threshold off
  /// aircraftRadiusMiles rather than hardcoding a mile count.
  double radiusMiles = 10.0;
  /// Set on every non-Ok status, including Empty - what to put on screen.
  String message;
};

/// GETs /api/myaircraft/mine with the device's own secret and no id anywhere
/// in the request - identical authentication to Weather::fetchMine() and
/// every other request the App makes (see MyAircraftEndpoints.cs's remarks,
/// which mirror MyWeatherEndpoints.cs's own for why "mine" replaced an
/// id-bearing route).
Result fetchMine();

}  // namespace Aircraft
