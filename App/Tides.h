#pragma once

#include <Arduino.h>

/// The next high and low tide for the device's position, as a card - real
/// NOAA CO-OPS data, same structural shape as SunMoon.h, its nearest sibling:
///
/// **This card fetches nothing.** Everything it draws already arrives on the
/// check-in response the device makes anyway - `nextHighTideMinutesUtc` and
/// `nextLowTideMinutesUtc`, in minutes after UTC midnight, the same unit and
/// the same `utcOffsetMinutes` the sunrise/sunset card already uses. No
/// request, no timer, and no failure mode of its own here - the card is a
/// rendering of state the device is already given.
///
/// **Pushed in, not pulled**, for the same reason SunMoon::setTimes() is: the
/// check-in path is the only thing that knows these values, and it already
/// runs on its own cadence. See App.ino's performCheckIn(), which calls this
/// right alongside SunMoon::setTimes().
///
/// **Minutes, not timestamps** - the same `(value + utcOffsetMinutes + 1440) %
/// 1440` arithmetic SunMoon.cpp's own toLocalMinutes() uses, duplicated here
/// rather than shared across a header: each card module here owns its own
/// copy of this one-line conversion (see MoonPhase.cpp for the same pattern),
/// so there is no cross-card dependency for a formula this small.
///
/// **Absent is a real answer**, but a simpler one than SunMoon's. There is no
/// polar-style "does not happen today" case for a tide - the only way either
/// value is ever absent is no NOAA station within range of the resolved
/// position, no position resolved at all, or NOAA unreachable with nothing
/// cached. Rather than invent wording for what is really just "nothing to
/// show yet", this card reports zero items when *both* are absent (see
/// cardItemCount()) and drops out of the rotation, the same tolerance
/// MoonPhase.cpp's own card gets. When only one of the two is absent, the
/// other is still drawn and the missing one reads as "--:--" - a real,
/// visible gap rather than a fabricated time.
///
/// **UNVERIFIED ON HARDWARE.** Like SunMoon and MoonPhase, this firmware has
/// no automated tests. What is here is checked by a clean compile and by
/// reading.
namespace Tides {

/// This card's registered id, and the `id` a policy entry must use to
/// schedule it. Exposed so the id appears exactly once in the firmware.
extern const char* const kCardId;

/// Called from App.ino on every successful check-in with the values off the
/// response. -1 for either means "no such tide to report" - see the header
/// note above. Cheap and idempotent; safe to call on every check-in whether
/// or not anything changed.
void setTimes(int nextHighTideMinutesUtc, int nextLowTideMinutesUtc, int utcOffsetMinutes);

}  // namespace Tides
