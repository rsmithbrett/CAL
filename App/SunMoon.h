#pragma once

#include <Arduino.h>

/// Today's sunrise and sunset, as a card.
///
/// **This card fetches nothing.** Unlike Weather.h and Aircraft.h, which each
/// own a server route and a response shape, everything this draws already
/// arrives on the check-in response the device makes anyway:
/// `sunriseMinutesUtc` and `sunsetMinutesUtc`, in minutes after UTC midnight,
/// alongside the `utcOffsetMinutes` the corner clock already uses. So there is
/// no request, no timer, and no failure mode of its own here - the card is a
/// rendering of state the device is already given.
///
/// That is why the values are pushed in via setTimes() rather than pulled: the
/// check-in path is the only thing that knows them, and it already runs on its
/// own cadence.
///
/// **Minutes, not timestamps.** The server sends minutes after UTC midnight
/// specifically so this arithmetic is `(value + utcOffsetMinutes + 1440) % 1440`
/// - no date handling, no 64-bit epoch, no timezone table on the device. The
/// same reasoning as the corner clock's, and it means this card needs no notion
/// of timezone any more than the clock does.
///
/// **Absent is a real answer.** -1 means the server had no sunrise or sunset to
/// report, which happens in three genuinely different situations that all want
/// the same treatment here: polar day, polar night, and a device whose position
/// has never resolved. The card says so in words rather than rendering a
/// placeholder time, because "00:00" would be a plausible-looking lie.
///
/// **UNVERIFIED ON HARDWARE.** This firmware has no automated tests. What is
/// here is checked by a clean compile and by reading.
namespace SunMoon {

/// This card's registered id, and the `id` a policy entry must use to schedule
/// it. Exposed so the id appears exactly once in the firmware.
extern const char* const kCardId;

/// Called from App.ino on every successful check-in with the values off the
/// response. -1 for either means "no such time today" - see the header note.
/// Cheap and idempotent; safe to call on every check-in whether or not
/// anything changed.
void setTimes(int sunriseMinutesUtc, int sunsetMinutesUtc, int utcOffsetMinutes);

}  // namespace SunMoon
