#include "HomeValue.h"

#include <time.h>

#include "Cards.h"
#include "Display.h"
#include "Log.h"

namespace HomeValue {

const char* const kCardId = "homevalue";

namespace {

/// Negative means "nothing to show" - see HomeValue.h's own remarks. Retained
/// across check-ins for the same reason every other pushed card's globals
/// are: a value the server hands back on one check-in has to keep being true
/// in the gaps between them, not just for the loop() iteration it arrived on.
int gEstimatedValue = -1;
int gRangeLow = -1;
int gRangeHigh = -1;
double gPricePerSquareFoot = -1.0;
/// 0 means "no refresh timestamp to show" - see HomeValue.h.
time_t gUpdatedAtUtc = 0;

/// Logged only on a change of the headline estimate, the same "don't spam
/// the remote debug stream every check-in" reasoning Tides.cpp's
/// gLastLoggedHighTide/gLastLoggedLowTide use. -2 rather than -1 so the very
/// first real -1 (an owner with no answer) still logs once instead of
/// matching this starting value by coincidence.
int gLastLoggedEstimate = -2;

bool hasEstimate() { return gEstimatedValue >= 0; }
bool hasRange() { return gRangeLow >= 0 && gRangeHigh >= 0; }
bool hasPricePerSquareFoot() { return gPricePerSquareFoot >= 0.0; }
bool hasUpdatedAt() { return gUpdatedAtUtc > 0; }

/// Comma-grouped whole-dollar figure ("$425,000") - manual grouping since
/// ESP32 libc locale support is not to be relied on, the same reasoning
/// Display.cpp's own Listings-facing formatPrice() gives for its identical
/// technique. Duplicated here rather than shared across a header - see
/// Tides.cpp's own remarks on why this codebase keeps small per-card
/// formatting helpers local rather than reaching across modules for one this
/// small.
String formatDollars(int amount) {
  const String digits = String(amount);
  String grouped;
  int sinceComma = 0;
  for (int i = digits.length() - 1; i >= 0; --i) {
    grouped = digits[i] + grouped;
    sinceComma++;
    if (sinceComma % 3 == 0 && i > 0) {
      grouped = "," + grouped;
    }
  }
  return "$" + grouped;
}

/// Rounded to the nearest thousand ("$400K") rather than comma-grouped in
/// full like the headline estimate above. The confidence range is
/// necessarily approximate context, not the number itself, and the two
/// full-precision comma-grouped figures this card's "Range" row would
/// otherwise need ("$400,000 - $450,000") do not fit this card's
/// label-left/value-right stat row next to its own "Range" label without
/// either colliding with it or being silently character-truncated into a
/// wrong number by Display.cpp's drawRightJustified() - truncation that is
/// merely cosmetic for a long airline name is not acceptable for a dollar
/// figure. Rounding instead of truncating keeps the value honest at the
/// precision this row actually has room for.
String formatThousands(int amount) {
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "$%dK", (amount + 500) / 1000);
  return String(buffer);
}

String estimateText() { return hasEstimate() ? formatDollars(gEstimatedValue) : "--"; }

String rangeText() {
  return hasRange() ? (formatThousands(gRangeLow) + " - " + formatThousands(gRangeHigh)) : "--";
}

String pricePerSquareFootText() {
  if (!hasPricePerSquareFoot()) {
    return "--";
  }
  char buffer[24];
  snprintf(buffer, sizeof(buffer), "$%.0f/sq ft", gPricePerSquareFoot);
  return String(buffer);
}

/// Local "Mon DD" for a UTC epoch instant - the same shift-then-gmtime_r
/// idiom IssFlyover.cpp's localHhMm() uses for a time-of-day and ClockDate.cpp
/// uses for "now", just formatted as a calendar date instead of a clock face:
/// RentCast refreshes on a multi-day cycle (see HomeValue.h), so a
/// day-granularity date says more here than a time of day would.
/// Display::utcOffsetMinutes() rather than a pushed copy of its own, the same
/// choice ClockDate.cpp makes and for the same reason given there: Display.cpp
/// is already the one place tracking "what does the App currently believe
/// about local time", and a third copy of that belief could only drift from
/// the two that already exist.
String updatedAtText() {
  if (!hasUpdatedAt()) {
    return "";
  }
  const time_t localEpoch = gUpdatedAtUtc + static_cast<time_t>(Display::utcOffsetMinutes()) * 60;
  struct tm localTm;
  gmtime_r(&localEpoch, &localTm);
  char buffer[16];
  strftime(buffer, sizeof(buffer), "%b %d", &localTm);
  return String(buffer);
}

/// The one line under the two stat rows that is not the fixed compliance
/// caption Display::showHomeValueCard() itself always draws - price per
/// square foot and the date RentCast was last actually asked, joined the
/// same "two related facts, one line" way showSunMoonCard()'s day-length
/// string stands in for a would-be third stat row. Empty when neither half
/// has an answer, so Display::showHomeValueCard() simply skips this line
/// rather than drawing a blank one.
String detailText() {
  String detail;
  if (hasPricePerSquareFoot()) {
    detail = pricePerSquareFootText();
  }
  const String updated = updatedAtText();
  if (updated.length() > 0) {
    detail += (detail.length() > 0 ? " - updated " : "Updated ") + updated;
  }
  return detail;
}

/// Nothing to fetch - see HomeValue.h. Present because CardSpec requires one
/// and the scheduler calls it; doing nothing here is the honest
/// implementation rather than an oversight.
void cardFetch() {}

/// One item once the server has a real headline estimate to show, none
/// before - the same "report zero rather than invent wording" tolerance
/// Tides.cpp's and MoonPhase.cpp's own cards get. The range and
/// price-per-square-foot figures do not gate this on their own; see
/// HomeValue.h's own remarks on why they are tolerated independently absent.
uint16_t cardItemCount() { return hasEstimate() ? 1 : 0; }

void cardDraw(uint16_t) {
  const String estimate = estimateText();
  const String range = rangeText();
  const String perSqFt = pricePerSquareFootText();

  // A second, quieter logging tier than the on-change gLastLoggedEstimate
  // line in setValue() below - this runs on every draw (once per dwell cycle
  // while this card is in rotation, not once per check-in) and states
  // exactly what is currently on screen, so someone tailing the remote debug
  // stream can reconstruct this card's content without standing in front of
  // the actual device. A no-op unless streaming is currently on for this
  // device - see Log::verbose()'s own remarks on why that gate matters for a
  // call site this frequent.
  Log::verbose("[homevalue] on screen: estimate=%s range=%s perSqFt=%s", estimate.c_str(),
               range.c_str(), perSqFt.c_str());

  Display::showHomeValueCard(estimate, range, detailText());
}

// ---------------------------------------------------------------------------
// The card descriptor.
//
// Interstitial, grouped with sunmoon/moonphase/tides/announcement at order 4:
// like them, this is one fixed fact rather than a feed, so "show after every
// N other cards" describes it honestly.
//
// dwellSeconds=10 and interleaveEvery=6 match tides'/sunmoon's own cadence:
// RentCast refreshes server-side on a multi-day cycle (see HomeValue.h), the
// same "changes at most daily" timescale sunrise/sunset and the tide clock
// already earn that cadence for - there is no reason for a household to see
// this less often than either of those two, or as often as something that
// actually changes within the day.
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

void setValue(int estimatedValue, int rangeLow, int rangeHigh, double pricePerSquareFoot,
              time_t updatedAtUtc) {
  gEstimatedValue = estimatedValue;
  gRangeLow = rangeLow;
  gRangeHigh = rangeHigh;
  gPricePerSquareFoot = pricePerSquareFoot;
  gUpdatedAtUtc = updatedAtUtc;

  if (estimatedValue != gLastLoggedEstimate) {
    gLastLoggedEstimate = estimatedValue;
    if (hasEstimate()) {
      Log::printf("[homevalue] estimate=%s range=%s perSqFt=%s updated=%s", estimateText().c_str(),
                  rangeText().c_str(), pricePerSquareFootText().c_str(),
                  hasUpdatedAt() ? updatedAtText().c_str() : "--");
    } else {
      Log::line("[homevalue] server reported no home value estimate for this device");
    }
  }
}

}  // namespace HomeValue
