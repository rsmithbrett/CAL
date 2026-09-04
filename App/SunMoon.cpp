#include "SunMoon.h"

#include "Cards.h"
#include "Display.h"
#include "Log.h"

namespace SunMoon {

const char* const kCardId = "sunmoon";

namespace {

/// Minutes after UTC midnight, or -1 for "no such time today". Retained across
/// check-ins for the same reason App.ino retains the UTC offset: a value the
/// server hands back on one check-in has to keep being true in the gaps
/// between them, not just for the loop() iteration it arrived on.
int gSunriseMinutesUtc = -1;
int gSunsetMinutesUtc = -1;
int gUtcOffsetMinutes = 0;

/// Whether a check-in has ever landed. Distinguishes "the server says there is
/// no sunrise today" from "nobody has told us anything yet", which the two -1s
/// alone cannot - see cardItemCount().
bool gHasCheckedIn = false;

/// Logged only on a change. setTimes() runs on every check-in, so logging
/// unconditionally would put the same line in the remote debug stream every
/// five minutes forever.
int gLastLoggedSunrise = -2;
int gLastLoggedSunset = -2;

constexpr int kMinutesPerDay = 1440;

bool hasTimes() { return gSunriseMinutesUtc >= 0 && gSunsetMinutesUtc >= 0; }

/// The whole of the timezone handling on this device: add the offset the server
/// already computed and wrap. The + kMinutesPerDay before the modulo is what
/// keeps a negative offset (every western timezone) from producing a negative
/// remainder - C's % is not Python's, and -300 % 1440 is -300, not 1140.
int toLocalMinutes(int utcMinutes) {
  return ((utcMinutes + gUtcOffsetMinutes) % kMinutesPerDay + kMinutesPerDay) % kMinutesPerDay;
}

String formatHhMm(int minutesOfDay) {
  char buffer[6];
  snprintf(buffer, sizeof(buffer), "%02d:%02d", minutesOfDay / 60, minutesOfDay % 60);
  return String(buffer);
}

/// Sunset minus sunrise, in UTC, wrapped so a day that crosses UTC midnight
/// still measures correctly. Computed in UTC deliberately: the offset shifts
/// both ends equally, so applying it first would change nothing and only add a
/// way to get it wrong.
String dayLength() {
  int minutes = gSunsetMinutesUtc - gSunriseMinutesUtc;
  if (minutes < 0) {
    minutes += kMinutesPerDay;
  }
  char buffer[40];
  snprintf(buffer, sizeof(buffer), "%dh %dm of daylight", minutes / 60, minutes % 60);
  return String(buffer);
}

/// Nothing to fetch - see SunMoon.h. Present because CardSpec requires one and
/// the scheduler calls it; doing nothing here is the honest implementation
/// rather than an oversight.
void cardFetch() {}

/// One item whenever the server has given this device a sunrise and a sunset,
/// and none otherwise.
///
/// Reporting zero in the polar and unresolved cases would remove the card from
/// the rotation entirely, which is deliberately NOT what happens: "the Sun does
/// not set today" is real, interesting content for the household that is
/// actually living through it, and an unresolved position is worth seeing
/// rather than hiding. So this reports one item as long as the device has heard
/// from the server at all, and draw() explains which case it is in.
///
/// The one state that genuinely has nothing to say is a device that has never
/// completed a check-in, where -1 means "not asked yet" rather than "no
/// sunrise". That is indistinguishable here from a polar day, so the card
/// stays out of the rotation until the first check-in lands - a few seconds
/// after boot - rather than briefly claiming the Sun does not rise.
uint16_t cardItemCount() { return gHasCheckedIn ? 1 : 0; }

void cardDraw(uint16_t) {
  if (hasTimes()) {
    Display::showSunMoonCard(
        formatHhMm(toLocalMinutes(gSunriseMinutesUtc)),
        formatHhMm(toLocalMinutes(gSunsetMinutesUtc)),
        dayLength());
    return;
  }

  // No times, but the device has checked in - so this is polar day, polar
  // night, or an unresolved position. The device cannot tell those apart from
  // the two nulls alone, so the wording covers all three without claiming
  // which: em-dashes rather than a fabricated "00:00".
  Display::showSunMoonCard(
      "--:--", "--:--",
      "No sunrise or sunset today for this location.");
}

// ---------------------------------------------------------------------------
// The card descriptor.
//
// Interstitial rather than list, the same reasoning as Graphic.cpp: this is one
// fixed fact per day, not a feed, so "show after every N other cards" describes
// it honestly. A list card would take a fixed slot in the sequence and be seen
// proportionally less often as the aircraft list grew.
//
// Ordered after weather and aircraft and interleaved less often than either:
// sunrise and sunset change once a day, so showing them as often as live data
// would crowd out the cards that actually change. Every value here is a
// built-in default that holds only until the first cardPolicy replaces it.
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
  spec.interleaveEvery = 6;
  return Cards::registerCard(spec);
}();

}  // namespace

void setTimes(int sunriseMinutesUtc, int sunsetMinutesUtc, int utcOffsetMinutes) {
  gSunriseMinutesUtc = sunriseMinutesUtc;
  gSunsetMinutesUtc = sunsetMinutesUtc;
  gUtcOffsetMinutes = utcOffsetMinutes;
  gHasCheckedIn = true;

  if (sunriseMinutesUtc != gLastLoggedSunrise || sunsetMinutesUtc != gLastLoggedSunset) {
    gLastLoggedSunrise = sunriseMinutesUtc;
    gLastLoggedSunset = sunsetMinutesUtc;
    if (hasTimes()) {
      Log::printf("[sunmoon] sunrise=%s sunset=%s local (offset %d min)",
                  formatHhMm(toLocalMinutes(sunriseMinutesUtc)).c_str(),
                  formatHhMm(toLocalMinutes(sunsetMinutesUtc)).c_str(), utcOffsetMinutes);
    } else {
      Log::line("[sunmoon] server reported no sunrise/sunset for this location today");
    }
  }
}

}  // namespace SunMoon
