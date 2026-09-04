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
/// (Route.h, via hexdb.io). That used to have no equivalent here; the server
/// now does the same resolution fleet-wide instead of per-device (see
/// DiscoverAroundMe's "Give the aircraft card its airline, logo and route
/// back" - the airline directory, hexdb.io route/airport cache, and seven new
/// AircraftSighting fields), so this module and Display::showAircraftCard()
/// consume them the same way this card already consumed the original five.
///
/// All seven new fields are optional on the wire and are read that way here -
/// see Sighting's own remarks below for what an absent value of each one
/// means and what this card does about it. None of them are ever required
/// for the original five-field card to keep working exactly as it always
/// has: a server old enough to predate this enrichment simply never sends
/// them, ArduinoJson's `| ""` idiom reads that as empty, and every draw path
/// below already treats empty as "fall back to the old behaviour".
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

  /// The matched directory row's code - a real ICAO prefix, or the
  /// server's two non-ICAO catch-alls "PVT" (general aviation, a tail
  /// number rather than an airline prefix) and "OTH" (a real airline
  /// prefix the directory doesn't have a row for). Not drawn directly by
  /// this card; airlineName below is what appears on screen; this exists so
  /// firmware logic could branch on it later without needing to add a wire
  /// field for that. Empty only against firmware old enough to predate this
  /// field entirely - a server that has it always sends one of the three.
  String airlineCode;
  /// The directory row's display name, already chosen to be card-width by
  /// whoever maintains the directory server-side - drawn verbatim, never
  /// wrapped or re-derived here. Empty means exactly one thing: this
  /// firmware is talking to a server old enough to not send it. It does NOT
  /// mean "general aviation" - PVT and OTH are ordinary directory rows with
  /// their own editable names, so a real server always sends something here
  /// once it has the field at all.
  String airlineName;
  /// An Assets catalog id, fetched the same way Graphic.cpp fetches its own
  /// configured picture - see Aircraft.cpp's cardFetch(). Empty means no
  /// logo is on file for this row, which is the ordinary case for any
  /// airline nobody has uploaded one for yet, not a fetch failure.
  String airlineLogoAssetId;
  /// ICAO airport codes ("KRDU"), not full names. The server also sends
  /// originName/destinationName; this card draws codes only and does not
  /// parse them - see Display::showAircraftCard()'s own remarks on why.
  /// Either code can be empty independently: originCode set with
  /// destinationCode empty is a filed departure with no filed arrival
  /// (common for general aviation and some regional traffic); both empty is
  /// "hexdb has nothing on file for this callsign", itself the common case
  /// for GA and short-hop regional flights, not a lookup failure.
  String originCode;
  String destinationCode;
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
