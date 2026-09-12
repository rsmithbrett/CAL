#include "IssFlyover.h"

#include <math.h>
#include <time.h>

#include "Cards.h"
#include "Display.h"
#include "Log.h"

namespace IssFlyover {

const char* const kCardId = "issflyover";

namespace {

/// Negative distance means "nothing to show" - see IssFlyover.h's own
/// remarks on why this is a safe absence gate for all four fields together.
/// Latitude/longitude default to a value outside any real coordinate purely
/// so an accidental read before the first setPosition() call reads as
/// obviously wrong rather than as the Gulf of Guinea.
double gLatitude = -999.0;
double gLongitude = -999.0;
double gDistanceMiles = -1.0;
double gBearingDegrees = -1.0;

/// Logged only on a change of state, the same "don't spam the remote debug
/// stream every check-in" reasoning Tides.cpp's gLastLoggedHighTide/
/// gLastLoggedLowTide use.
bool gLastLoggedHadData = false;

/// The next predicted pass - see IssFlyover.h and CheckIn.h's own
/// issNextPassRiseUtc remarks. 0 for the two epochs means "no pass to
/// show", the same out-of-range-sentinel reasoning gLatitude/gLongitude
/// above use; -1.0 for the azimuth/elevation fields matches gBearingDegrees'
/// own convention.
time_t gNextPassRiseUtc = 0;
double gNextPassRiseAzimuthDegrees = -1.0;
time_t gNextPassMaxElevationUtc = 0;
double gNextPassMaxElevationDegrees = -1.0;
double gNextPassMaxElevationAzimuthDegrees = -1.0;
time_t gNextPassSetUtc = 0;
double gNextPassSetAzimuthDegrees = -1.0;
int gNextPassUtcOffsetMinutes = 0;

/// Logged only on a change of state, same reasoning as gLastLoggedHadData.
bool gLastLoggedHadNextPass = false;

bool hasData() { return gDistanceMiles >= 0.0; }

/// A pass counts as "present" only while it has not already happened - see
/// IssFlyover.h's own remarks on why this card never keeps showing a pass
/// whose predicted set time is already in the past just because the device
/// has not checked in again since. time(nullptr) and the two epochs here are
/// all the same absolute-UTC-seconds unit, so this comparison needs no
/// local-offset arithmetic at all - that only enters when formatting a time
/// for display, not when deciding whether one is still upcoming.
bool hasNextPass() {
  return gNextPassRiseUtc > 0 && gNextPassSetUtc > 0 && time(nullptr) < gNextPassSetUtc;
}

/// Nothing to fetch - see IssFlyover.h. Present because CardSpec requires
/// one and the scheduler calls it.
void cardFetch() {}

/// One item once either a next pass or the live position has real data to
/// show, none before - the same "report zero rather than invent wording"
/// tolerance Tides.cpp's and MoonPhase.cpp's own cards get. cardDraw() below
/// decides which of the two this actually draws, preferring the next pass
/// whenever both happen to be present at once.
uint16_t cardItemCount() { return (hasNextPass() || hasData()) ? 1 : 0; }

/// 8-point compass, matched to the nearest 45-degree sector - plenty of
/// precision for "which way to look", which is all this card claims to
/// answer (see IssFlyover.h and CheckInModels.cs's own remarks on why this
/// is a map bearing, not a real observational azimuth).
const char* compassPoint(double bearingDegrees) {
  static const char* const kPoints[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  const int index = static_cast<int>((bearingDegrees + 22.5) / 45.0) % 8;
  return kPoints[index < 0 ? index + 8 : index];
}

String distanceText() {
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%.0f mi", gDistanceMiles);
  return String(buffer);
}

/// "deg" rather than a degree glyph - Display::drawTemperature() draws its
/// own degree mark as a small graphical ring rather than a text character,
/// which is this codebase's own evidence that the panel's bitmap fonts do
/// not reliably carry a "\xB0" glyph. A stat-row value has no room for a
/// second graphical primitive the way that big temperature numeral does, so
/// spelling it out is the safe choice here rather than risking a blank box.
String directionText() {
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%03.0f deg %s", gBearingDegrees, compassPoint(gBearingDegrees));
  return String(buffer);
}

/// "38.9N, 77.0W" rather than signed decimals - a household reading this
/// card is thinking in compass terms already (see directionText() above),
/// not in the signed-longitude convention a mapping API uses internally.
String coordinateText() {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "Over %.1f%s, %.1f%s", fabs(gLatitude),
           gLatitude >= 0 ? "N" : "S", fabs(gLongitude), gLongitude >= 0 ? "E" : "W");
  return String(buffer);
}

/// Local "HH:MM" for a UTC epoch instant - the same shift-then-gmtime_r
/// idiom ClockDate.cpp uses for the current time (`time(nullptr) +
/// utcOffsetMinutes*60` then gmtime_r), just applied to an arbitrary
/// already-known instant instead of "now".
String localHhMm(time_t utcEpoch) {
  const time_t localEpoch = utcEpoch + static_cast<time_t>(gNextPassUtcOffsetMinutes) * 60;
  struct tm localTm;
  gmtime_r(&localEpoch, &localTm);
  // Through Display so a household that reads 12-hour clocks sees one here too -
  // Display::formatTimeOfDay() is the only place a time of day becomes text on
  // this device.
  return Display::formatTimeOfDay(localTm.tm_hour, localTm.tm_min);
}

String nextPassRiseTimeText() { return localHhMm(gNextPassRiseUtc); }

/// Same "042 deg NE" shape as directionText() above, for the rise azimuth.
String nextPassRiseDirectionText() {
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%03.0f deg %s", gNextPassRiseAzimuthDegrees,
            compassPoint(gNextPassRiseAzimuthDegrees));
  return String(buffer);
}

/// The two facts that do not fit in the two stat rows: how high the pass
/// gets (the single best signal for "is this one worth going outside for")
/// and when it is over - the same "extra context in the detail line" role
/// showSunMoonCard()'s day-length string and this card's own coordinateText()
/// already play.
String nextPassDetailText() {
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "Highest %.0f deg %s at %s, sets %s %s",
            gNextPassMaxElevationDegrees, compassPoint(gNextPassMaxElevationAzimuthDegrees),
            localHhMm(gNextPassMaxElevationUtc).c_str(), localHhMm(gNextPassSetUtc).c_str(),
            compassPoint(gNextPassSetAzimuthDegrees));
  return String(buffer);
}

/// Re-asserted once per check-in - see Cards.h's StatusFn. Reports both halves
/// separately because they fail independently: the server can have a live
/// position but no upcoming visible pass (the common case - most geometric
/// passes happen in daylight and are filtered out server-side), or a pass but
/// no fresh position.
String cardStatus() {
  String state = hasNextPass()
                     ? String("next pass ") + nextPassRiseTimeText() + " " + nextPassRiseDirectionText()
                     : String("no upcoming visible pass");
  state += hasData() ? String(", position ") + distanceText() : String(", no live position");
  return state;
}

/// The next pass wins whenever there is one, and the live position is the
/// fallback rather than the other way round.
///
/// This reverses the original preference, on the product owner's own framing:
/// "change the display to when the next time the space station is visible, and
/// the details". The live position is genuine data but it is almost never
/// actionable - the station spends most of its orbit over ocean, thousands of
/// miles away, and "11,545 mi, 212 deg SW, over 49.5S 114.1E" invites nobody
/// to do anything. "Rises 22:10 to the NW, highest 16 deg, sets 22:14" gets
/// someone outside, which is the entire point of putting the ISS on a wall
/// display. Note the server now only ever sends a pass that is actually
/// VISIBLE - observer in darkness, station in sunlight - so a pass reaching
/// this card is one worth acting on (see the server's IssPassVisibility).
///
/// The live position keeps its place underneath because it is the honest
/// answer when there is no upcoming pass at all: some latitudes go days
/// between passes while the orbital plane precesses back overhead, and
/// "where is it right now" beats an empty card during that gap.
void cardDraw(uint16_t) {
  if (hasNextPass()) {
    Log::verbose("[issflyover] drawing next-pass prediction: rises %s (%s)",
                nextPassRiseTimeText().c_str(), nextPassRiseDirectionText().c_str());
    Display::showIssNextPassCard(nextPassRiseTimeText(), nextPassRiseDirectionText(),
                                  nextPassDetailText());
    return;
  }
  // No upcoming pass, but cardItemCount() only let this run at all because
  // hasData() is true - see IssFlyover.h's "second display mode" remarks.
  Log::verbose("[issflyover] drawing live position: %s, %s (%s)", distanceText().c_str(),
              directionText().c_str(), coordinateText().c_str());
  Display::showIssFlyoverCard(distanceText(), directionText(), coordinateText());
}

// ---------------------------------------------------------------------------
// The card descriptor.
//
// Interstitial, grouped with sunmoon/tides/moonphase/announcement at order
// 4: like them, this is one fixed fact recomputed on check-in rather than a
// feed, so "show after every N other cards" describes it honestly.
//
// interleaveEvery 9, matching qrtext's own cadence - a fixed fact that
// changes meaningfully within minutes (the ISS moves roughly 7.66 km/s)
// still only updates once per check-in interval here, so there is no
// argument for showing it more often than the other order-4 siblings.
// ---------------------------------------------------------------------------
[[maybe_unused]] const bool kRegistered = [] {
  Cards::CardSpec spec;
  spec.id = kCardId;
  spec.kind = Cards::Kind::Interstitial;
  spec.fetch = cardFetch;
  spec.itemCount = cardItemCount;
  spec.draw = cardDraw;
  spec.status = cardStatus;
  spec.order = 4;
  spec.dwellSeconds = 10;
  spec.interleaveEvery = 9;
  return Cards::registerCard(spec);
}();

}  // namespace

void setPosition(double latitude, double longitude, double distanceMiles, double bearingDegrees) {
  gLatitude = latitude;
  gLongitude = longitude;
  gDistanceMiles = distanceMiles;
  gBearingDegrees = bearingDegrees;

  const bool nowHasData = hasData();
  if (nowHasData != gLastLoggedHadData) {
    gLastLoggedHadData = nowHasData;
    if (nowHasData) {
      Log::printf("[issflyover] %s, %s (%s)", distanceText().c_str(), directionText().c_str(),
                  coordinateText().c_str());
    } else {
      Log::line("[issflyover] server reported no ISS position for this device");
    }
  }
}

void setNextPass(time_t riseUtc, double riseAzimuthDegrees, time_t maxElevationUtc,
                  double maxElevationDegrees, double maxElevationAzimuthDegrees, time_t setUtc,
                  double setAzimuthDegrees, int utcOffsetMinutes) {
  gNextPassRiseUtc = riseUtc;
  gNextPassRiseAzimuthDegrees = riseAzimuthDegrees;
  gNextPassMaxElevationUtc = maxElevationUtc;
  gNextPassMaxElevationDegrees = maxElevationDegrees;
  gNextPassMaxElevationAzimuthDegrees = maxElevationAzimuthDegrees;
  gNextPassSetUtc = setUtc;
  gNextPassSetAzimuthDegrees = setAzimuthDegrees;
  gNextPassUtcOffsetMinutes = utcOffsetMinutes;

  const bool nowHasNextPass = hasNextPass();
  if (nowHasNextPass != gLastLoggedHadNextPass) {
    gLastLoggedHadNextPass = nowHasNextPass;
    if (nowHasNextPass) {
      Log::printf("[issflyover] next pass rises %s (%s), peaks %.0f deg near %s, sets %s",
                  nextPassRiseTimeText().c_str(), nextPassRiseDirectionText().c_str(),
                  gNextPassMaxElevationDegrees, localHhMm(gNextPassMaxElevationUtc).c_str(),
                  localHhMm(gNextPassSetUtc).c_str());
    } else {
      Log::line("[issflyover] server reported no upcoming ISS pass for this device");
    }
  }
}

}  // namespace IssFlyover
