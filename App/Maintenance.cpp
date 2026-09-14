#include "Maintenance.h"

#include "Display.h"

namespace Maintenance {
namespace {

/// The declared deadline, UTC epoch seconds, 0 for "none held". See
/// Maintenance.h for why this lives here and not as an App.ino global, and why
/// it is never persisted.
time_t gWindowEndUtc = 0;

/// How far ahead a deadline may be and still be worth naming a clock time for.
///
/// Twelve hours is the point past which "back by 2:30" stops helping: a reader
/// glancing at a wall display has no way to tell today's 2:30 from tomorrow's,
/// and a window that long is a planned migration rather than the five-minute
/// deploy this feature was built around. Beyond it the card still says
/// maintenance - which is the useful half - and simply declines to put a time on
/// it, rather than printing one that reads as a promise about the wrong day.
constexpr long kMaxSecondsAheadToName = 12L * 60L * 60L;

/// Added before the deadline is truncated to a minute for display, so "back by"
/// is never EARLIER than the actual deadline. A window ending at 14:30:45 that
/// printed "back by 14:30" would be wrong in the one direction that matters:
/// promising the service sooner than it is coming. Rounding up costs at most 59
/// seconds of pessimism, which nobody reads as a broken promise.
constexpr time_t kRoundUpToMinute = 59;

}  // namespace

void setWindowEnd(time_t untilUtc) { gWindowEndUtc = untilUtc > 0 ? untilUtc : 0; }

time_t windowEnd() { return gWindowEndUtc; }

bool inWindow() {
  if (gWindowEndUtc <= 0) {
    return false;
  }
  // Identical test to the one App.ino's watchdog has always applied inline, kept
  // identical on purpose: `nowUtc > 0` is what rules out a device whose clock has
  // not been set yet, and any deadline that survived CheckIn.cpp's
  // parseMaintenanceUntil() is already past Config::kEarliestPlausibleTime - so
  // an unset clock fails the comparison below regardless, and a second plausibility
  // floor here would only be a copy of that one.
  const time_t nowUtc = time(nullptr);
  return nowUtc > 0 && nowUtc < gWindowEndUtc;
}

long secondsRemaining() {
  if (!inWindow()) {
    return 0;
  }
  return static_cast<long>(gWindowEndUtc - time(nullptr));
}

String failureText(const String& plainText, bool serviceUnreachable) {
  // The overwhelmingly common path, and the one the whole design is answerable
  // for: no window held, or this failure is not the kind a downed server
  // explains, and the caller gets its own string straight back untouched.
  if (!serviceUnreachable || !inWindow()) {
    return plainText;
  }

  const time_t nowUtc = time(nullptr);
  if (gWindowEndUtc - nowUtc > kMaxSecondsAheadToName) {
    return String("Server maintenance in progress.");
  }

  // The same local-time formula CheckIn.h documents and Display.cpp's drawClock()
  // and ClockDate.cpp both already use - reached through Display::utcOffsetMinutes()
  // rather than kept as a fourth copy of the offset, and rendered through
  // Display::formatTimeOfDay() so this line honours the household's 12-or-24-hour
  // preference exactly like every other time this device prints.
  const time_t offsetSeconds = static_cast<time_t>(Display::utcOffsetMinutes()) * 60;
  const time_t localNow = nowUtc + offsetSeconds;
  const time_t localEnd = gWindowEndUtc + kRoundUpToMinute + offsetSeconds;

  struct tm nowTm;
  struct tm endTm;
  gmtime_r(&localNow, &nowTm);
  gmtime_r(&localEnd, &endTm);

  String text = "Server maintenance. Back by ";
  text += Display::formatTimeOfDay(endTm.tm_hour, endTm.tm_min);
  // Only ever one day out: the twelve-hour guard above already rejected anything
  // further, so a differing day-of-year here can only be the next one. tm_yday
  // rather than tm_mday so a window crossing midnight on the 31st, or crossing a
  // new year, still compares as "a different day" rather than 31 vs 1 looking
  // like a month's difference.
  if (endTm.tm_yday != nowTm.tm_yday) {
    text += " tomorrow";
  }
  text += ".";
  return text;
}

}  // namespace Maintenance
