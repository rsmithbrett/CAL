#pragma once

#include <Arduino.h>

/// Today's Moon phase, as a drawn disc rather than a text description - the
/// first of a new "graphical style" card family the product owner wants,
/// starting here. Same structural shape as SunMoon.h, its nearest sibling:
///
/// **This card fetches nothing.** Everything it draws already arrives on the
/// check-in response the device makes anyway - `moonPhase`,
/// `moonIlluminatedFraction` and `moonPhaseName` - alongside the sunrise/
/// sunset fields SunMoon.cpp already reads off the same response. No
/// request, no timer, and no failure mode of its own here.
///
/// **Pushed in, not pulled**, for the same reason SunMoon::setTimes() is:
/// the check-in path is the only thing that knows these values, and it
/// already runs on its own cadence. See App.ino's performCheckIn(), which
/// calls this right alongside SunMoon::setTimes().
///
/// **Absent is a real answer**, but a simpler one than SunMoon's. Sunrise/
/// sunset can be absent for three different reasons (polar day, polar
/// night, or an unresolved position) and the card says which in words.
/// The Moon's phase has no polar-style "does not happen today" case at
/// all - it is a fact of the date, not of the observer's horizon - so the
/// only way it is ever absent is a device whose position has never
/// resolved. Rather than invent wording for a state that is really just
/// "nothing to show yet", this card reports zero items in that case (see
/// cardItemCount()) and drops out of the rotation, the same tolerance a
/// graphic card with no assetId chosen already gets.
///
/// **UNVERIFIED ON HARDWARE.** Like SunMoon, this firmware has no automated
/// tests. What is here is checked by a clean compile and by reading -
/// Display::showMoonPhaseCard()'s own remarks record the rendering
/// technique and the Northern-Hemisphere waxing/waning convention chosen
/// for it in more detail.
namespace MoonPhase {

/// This card's registered id, and the `id` a policy entry must use to
/// schedule it. Exposed so the id appears exactly once in the firmware.
extern const char* const kCardId;

/// Called from App.ino on every successful check-in with the values off the
/// response. `phase` and `illuminatedFraction` negative (server sends no
/// answer, or a device that has never checked in) means "nothing to show" -
/// see the header note above. Cheap and idempotent; safe to call on every
/// check-in whether or not anything changed.
void setPhase(double phase, double illuminatedFraction, const String& phaseName);

}  // namespace MoonPhase
