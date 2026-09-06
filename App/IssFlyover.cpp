#include "IssFlyover.h"

#include <math.h>

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

bool hasData() { return gDistanceMiles >= 0.0; }

/// Nothing to fetch - see IssFlyover.h. Present because CardSpec requires
/// one and the scheduler calls it.
void cardFetch() {}

/// One item once real data has arrived, none before - the same "report zero
/// rather than invent wording" tolerance Tides.cpp's and MoonPhase.cpp's own
/// cards get.
uint16_t cardItemCount() { return hasData() ? 1 : 0; }

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

void cardDraw(uint16_t) {
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

}  // namespace IssFlyover
