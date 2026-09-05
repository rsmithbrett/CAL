#include "MoonPhase.h"

#include "Cards.h"
#include "Display.h"
#include "Log.h"

namespace MoonPhase {

const char* const kCardId = "moonphase";

namespace {

/// Negative means "nothing to show" - see MoonPhase.h's own remarks on why
/// this card's absent case is simpler than SunMoon's: there is no polar-style
/// "does not happen today", only a position that has never resolved.
double gPhase = -1.0;
double gIlluminatedFraction = -1.0;
String gPhaseName;

/// Logged only on a change of phase name, the same "don't spam the remote
/// debug stream every check-in" reasoning SunMoon.cpp's gLastLoggedSunrise/
/// gLastLoggedSunset use. Starts empty so the very first real answer always
/// logs once.
String gLastLoggedPhaseName;
bool gLastLoggedHadData = false;

bool hasData() { return gPhase >= 0.0 && gIlluminatedFraction >= 0.0; }

/// Nothing to fetch - see MoonPhase.h. Present because CardSpec requires one
/// and the scheduler calls it.
void cardFetch() {}

/// One item once real data has arrived, none before - the same
/// "gHasCheckedIn" gate SunMoon.cpp uses, but this card has no interesting
/// "checked in but nothing to say" state to report in words the way SunMoon's
/// polar day/night does, so it simply stays out of the rotation instead of
/// drawing a card with nothing on it. Matches the tolerance a graphic card
/// with no assetId chosen already gets: no configuration/data, no card.
uint16_t cardItemCount() { return hasData() ? 1 : 0; }

void cardDraw(uint16_t) {
  Display::showMoonPhaseCard(gPhaseName, gPhase, gIlluminatedFraction);
}

// ---------------------------------------------------------------------------
// The card descriptor.
//
// Interstitial, grouped with sunmoon and announcement at order 4: like them,
// this is one fixed daily fact rather than a feed, so "show after every N
// other cards" describes it honestly and it should not take a fixed slot the
// way a list card does.
//
// interleaveEvery is 7 - between sunmoon's 6 and announcement's 8. Sunrise
// and sunset change meaningfully once a day and are worth seeing about that
// often; the Moon's phase changes even more slowly (imperceptibly from one
// day to the next), so it earns a slightly longer gap than sunmoon, but it
// is still a fresh, illustrated card the product owner wants seen regularly
// rather than buried as rarely as a household notice - hence closer to
// sunmoon's cadence than announcement's.
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
  spec.interleaveEvery = 7;
  return Cards::registerCard(spec);
}();

}  // namespace

void setPhase(double phase, double illuminatedFraction, const String& phaseName) {
  gPhase = phase;
  gIlluminatedFraction = illuminatedFraction;
  gPhaseName = phaseName;

  const bool nowHasData = hasData();
  if (nowHasData && phaseName != gLastLoggedPhaseName) {
    gLastLoggedPhaseName = phaseName;
    gLastLoggedHadData = true;
    Log::printf("[moonphase] phase=%.3f illuminated=%.0f%% (%s)", phase,
                illuminatedFraction * 100.0, phaseName.c_str());
  } else if (!nowHasData && gLastLoggedHadData) {
    gLastLoggedHadData = false;
    gLastLoggedPhaseName = "";
    Log::line("[moonphase] server reported no Moon phase for this device (position unresolved)");
  }
}

}  // namespace MoonPhase
