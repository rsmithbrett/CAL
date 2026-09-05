#include "ClockDate.h"

#include <time.h>

#include "Cards.h"
#include "Display.h"

namespace ClockDate {

const char* const kCardId = "clockdate";

namespace {

/// Nothing to fetch - see ClockDate.h. Present because CardSpec requires a
/// FetchFn and the scheduler calls it on its own refresh timer regardless;
/// SunMoon.cpp's cardFetch() is the same no-op for the same reason.
void cardFetch() {}

/// Always one item - see ClockDate.h's own remarks on why this card, unlike
/// every other one registered so far, has no "not configured" or
/// "haven't checked in yet" state worth reporting zero for. There is always
/// a time to show.
uint16_t cardItemCount() { return 1; }

void cardDraw(uint16_t) {
  // The exact formula CheckIn.h documents and Display.cpp's drawClock()
  // already uses for the corner clock - see ClockDate.h for why this module
  // computes it again here rather than Display.cpp handing over an
  // already-computed time_t.
  const time_t localNow =
      time(nullptr) + static_cast<time_t>(Display::utcOffsetMinutes()) * 60;
  struct tm localTm;
  gmtime_r(&localNow, &localTm);

  char timeBuffer[6];
  snprintf(timeBuffer, sizeof(timeBuffer), "%02d:%02d", localTm.tm_hour, localTm.tm_min);

  // Weekday and month spelled out, the way a wall clock's own calendar strip
  // would read them, rather than the numeric HH:MM this device already shows
  // everywhere else. strftime() is already used for exactly this tm-to-string
  // step by CheckIn.cpp's nowAsIso8601Utc(); %A/%B render in the C locale's
  // English names, matching every other string this firmware puts on screen -
  // nothing here is localised, by design or otherwise.
  char dateBuffer[32];
  strftime(dateBuffer, sizeof(dateBuffer), "%A, %B %d", &localTm);

  Display::showClockDate(String(timeBuffer), String(dateBuffer));
}

// ---------------------------------------------------------------------------
// The card descriptor.
//
// Interstitial, the same reasoning as Graphic.cpp and SunMoon.cpp: a clock
// face is one fact, not a feed, so "show after every N other cards" is the
// honest cadence for it rather than a fixed slot that would be seen
// proportionally less often as a list card's own item count grows.
//
// order=5, after weather (implicitly first), aircraft, graphic (order=3) and
// sunmoon (order=4) - a decoration and a daily almanac fact both come before
// a clock face in the interstitial tie-break, since check-in-driven content
// is what this device exists to show and the clock is always available as a
// filler beat between them regardless of what the server has to say today.
//
// dwellSeconds=10 matches Graphic's and SunMoon's own interstitial default -
// long enough to actually read a clock face from across a room, short enough
// not to dominate the rotation.
//
// interleaveEvery=10, the least frequent of the three interstitials
// registered so far (graphic=8, sunmoon=6). Unlike a picture or a sunrise
// time, a clock is already visible - smaller, but always current - in the
// corner of every single card via Display.cpp's own drawClock(), so this
// full-screen version earns a rarer cadence than content that otherwise has
// no presence on screen at all.
// ---------------------------------------------------------------------------
[[maybe_unused]] const bool kRegistered = [] {
  Cards::CardSpec spec;
  spec.id = kCardId;
  spec.kind = Cards::Kind::Interstitial;
  spec.fetch = cardFetch;
  spec.itemCount = cardItemCount;
  spec.draw = cardDraw;
  spec.order = 5;
  spec.dwellSeconds = 10;
  spec.interleaveEvery = 10;
  return Cards::registerCard(spec);
}();

}  // namespace

}  // namespace ClockDate
