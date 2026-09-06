#pragma once

#include <Arduino.h>

/// Distance and compass direction to the International Space Station's
/// current sub-satellite point, as a card - real open-notify.org data, same
/// structural shape as Tides.h, its nearest sibling:
///
/// **This card fetches nothing.** Everything it draws already arrives on the
/// check-in response the device makes anyway - `issLatitude`, `issLongitude`,
/// `issDistanceMiles` and `issBearingDegrees` - the same "pushed in, not
/// pulled" contract Tides.cpp's own nextHighTideMinutesUtc/
/// nextLowTideMinutesUtc get and for the same reason: the check-in path is
/// the only thing that knows these values, and it already runs on its own
/// cadence. See App.ino's performCheckIn(), which calls this right alongside
/// Tides::setTimes().
///
/// **Absent is a real answer**, and for the same three reasons
/// SpaceStationResult.None documents server-side: open-notify has never been
/// reached, the cached position is too stale to trust, or this device's own
/// position never resolved. There is no polar-style partial-absence case
/// here the way sunrise/sunset has - either all four values are real or none
/// are - so this card reports zero items when they are absent (see
/// cardItemCount()) and drops out of the rotation, the same tolerance
/// MoonPhase.cpp's and Tides.cpp's own cards get.
///
/// **UNVERIFIED ON HARDWARE.** Like every other check-in-driven card in this
/// build, this firmware has no automated tests. What is here is checked by a
/// clean compile and by reading.
namespace IssFlyover {

/// This card's registered id, and the `id` a policy entry must use to
/// schedule it. Exposed so the id appears exactly once in the firmware.
/// Matches KnownCards.cs's "issflyover" on the server side exactly - a
/// mismatch here is what "policy names unknown card" in the log means.
extern const char* const kCardId;

/// Called from App.ino on every successful check-in with the values off the
/// response. distanceMiles negative means "nothing to show" - see the header
/// note above; a real distance is never negative, so it alone is a safe
/// absence gate for all four fields together. Cheap and idempotent; safe to
/// call on every check-in whether or not anything changed.
void setPosition(double latitude, double longitude, double distanceMiles, double bearingDegrees);

}  // namespace IssFlyover
