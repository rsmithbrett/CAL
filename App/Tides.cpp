#include "Tides.h"

#include "Cards.h"
#include "Display.h"
#include "Log.h"

namespace Tides {

const char* const kCardId = "tides";

namespace {

/// Minutes after UTC midnight, or -1 for "no such tide to report". Retained
/// across check-ins for the same reason SunMoon.cpp's globals are: a value
/// the server hands back on one check-in has to keep being true in the gaps
/// between them, not just for the loop() iteration it arrived on.
int gNextHighTideMinutesUtc = -1;
int gNextLowTideMinutesUtc = -1;
int gUtcOffsetMinutes = 0;

/// Whether a check-in has ever landed. Distinguishes "the server has nothing
/// to report" from "nobody has told us anything yet" - see cardItemCount().
bool gHasCheckedIn = false;

/// Logged only on a change. setTimes() runs on every check-in, so logging
/// unconditionally would put the same line in the remote debug stream every
/// five minutes forever.
int gLastLoggedHighTide = -2;
int gLastLoggedLowTide = -2;

constexpr int kMinutesPerDay = 1440;

bool hasHighTide() { return gNextHighTideMinutesUtc >= 0; }
bool hasLowTide() { return gNextLowTideMinutesUtc >= 0; }
bool hasAnyTide() { return hasHighTide() || hasLowTide(); }

/// The same `(value + offset + 1440) % 1440` arithmetic SunMoon.cpp's own
/// toLocalMinutes() uses - see Tides.h's own remarks on why this is
/// duplicated here rather than shared. The + kMinutesPerDay before the modulo
/// is what keeps a negative offset (every western timezone) from producing a
/// negative remainder - C's % is not Python's, and -300 % 1440 is -300, not
/// 1140.
int toLocalMinutes(int utcMinutes) {
  return ((utcMinutes + gUtcOffsetMinutes) % kMinutesPerDay + kMinutesPerDay) % kMinutesPerDay;
}

String formatHhMm(int minutesOfDay) {
  char buffer[6];
  snprintf(buffer, sizeof(buffer), "%02d:%02d", minutesOfDay / 60, minutesOfDay % 60);
  return String(buffer);
}

/// "--:--" for whichever tide is absent, rather than a fabricated time - the
/// same em-dash convention SunMoon.cpp uses for a polar day/night.
String highTideText() {
  return hasHighTide() ? formatHhMm(toLocalMinutes(gNextHighTideMinutesUtc)) : "--:--";
}

String lowTideText() {
  return hasLowTide() ? formatHhMm(toLocalMinutes(gNextLowTideMinutesUtc)) : "--:--";
}

/// Nothing to fetch - see Tides.h. Present because CardSpec requires one and
/// the scheduler calls it; doing nothing here is the honest implementation
/// rather than an oversight.
void cardFetch() {}

/// One item once at least one of the two tides has a real answer, none
/// before - the same "report zero rather than invent wording" tolerance
/// MoonPhase.cpp's card gets, not SunMoon's "always show one item and explain
/// which absent case this is": a tide has no polar-style story worth telling
/// in words, so when the server has nothing at all to report this card simply
/// stays out of the rotation instead of drawing a card with nothing on it.
uint16_t cardItemCount() { return (gHasCheckedIn && hasAnyTide()) ? 1 : 0; }

void cardDraw(uint16_t) {
  Display::showTidesCard(highTideText(), lowTideText());
}

// ---------------------------------------------------------------------------
// The card descriptor.
//
// Interstitial rather than list, the same reasoning as SunMoon.cpp: this is
// two fixed facts that change a handful of times a day, not a feed, so "show
// after every N other cards" describes it honestly.
//
// Grouped with sunmoon/moonphase/announcement at order 4, interleaving every
// 6 cards - the same cadence as sunmoon's, since a tide is exactly as
// worth seeing regularly as sunrise/sunset and changes on a similar
// timescale. Every value here is a built-in default that holds only until
// the first cardPolicy replaces it.
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

void setTimes(int nextHighTideMinutesUtc, int nextLowTideMinutesUtc, int utcOffsetMinutes) {
  gNextHighTideMinutesUtc = nextHighTideMinutesUtc;
  gNextLowTideMinutesUtc = nextLowTideMinutesUtc;
  gUtcOffsetMinutes = utcOffsetMinutes;
  gHasCheckedIn = true;

  if (nextHighTideMinutesUtc != gLastLoggedHighTide || nextLowTideMinutesUtc != gLastLoggedLowTide) {
    gLastLoggedHighTide = nextHighTideMinutesUtc;
    gLastLoggedLowTide = nextLowTideMinutesUtc;
    if (hasAnyTide()) {
      Log::printf("[tides] next high=%s next low=%s local (offset %d min)",
                  highTideText().c_str(), lowTideText().c_str(), utcOffsetMinutes);
    } else {
      Log::line("[tides] server reported no tide data for this location");
    }
  }
}

}  // namespace Tides
