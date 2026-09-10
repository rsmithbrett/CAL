#pragma once

#include <Arduino.h>

/// When the International Space Station is next visible overhead, and what to
/// look for - real CelesTrak/SGP4 orbital mechanics, same structural shape as
/// Tides.h, its nearest sibling. A second display mode sits underneath: when
/// there is no upcoming pass at all, the same card falls back to the station's
/// live position (distance and compass direction to its sub-satellite point,
/// from open-notify.org).
///
/// **The next pass leads and the live position is the fallback**, which is the
/// reverse of how this card originally worked. From the product owner: "change
/// the display to when the next time the space station is visible, and the
/// details". The live position is real data that is almost never actionable -
/// the station spends most of its orbit over open ocean, thousands of miles
/// away, and "11,545 mi, 212 deg SW" asks nobody to do anything. A pass time
/// gets someone outside to look up, which is the whole reason the ISS is worth
/// a card. The live position stays as the honest answer for the gap: some
/// latitudes go days between passes while the orbital plane precesses back
/// overhead, and "where is it right now" beats a blank card meanwhile.
///
/// **A pass reaching this card is one that can actually be SEEN.** The server
/// filters for visibility before sending it - observer in darkness and station
/// in sunlight, both required, because the ISS is only ever reflected sunlight
/// and roughly half of all geometric passes happen in daylight. See the
/// server's IssPassVisibility. This firmware does no visibility reasoning of
/// its own and must not start: it has neither the solar geometry nor the
/// observer's position to do it with.
///
/// **This card fetches nothing.** Everything it draws already arrives on the
/// check-in response the device makes anyway - `issLatitude`, `issLongitude`,
/// `issDistanceMiles`, `issBearingDegrees` for the live position, and the
/// seven `issNextPass*` fields for the pass prediction - the same "pushed
/// in, not pulled" contract Tides.cpp's own nextHighTideMinutesUtc/
/// nextLowTideMinutesUtc get and for the same reason: the check-in path is
/// the only thing that knows these values, and it already runs on its own
/// cadence. See App.ino's performCheckIn(), which calls setPosition() and
/// setNextPass() right alongside Tides::setTimes().
///
/// **Found live, wired in tonight**: the server side of pass prediction
/// (real CelesTrak/SGP4 orbital mechanics, see CheckInModels.cs's own
/// IssNextPassRiseUtc remarks) shipped earlier this same session, but nobody
/// ever taught this firmware to read the seven fields it added to the
/// check-in response - the household's own "space station is not reporting
/// when it will fly over" report is what caught it. setPosition() and this
/// card's live-position half are unchanged; setNextPass() and the fallback
/// display mode below are what that report was missing.
///
/// **Absent is a real answer**, and for the same three reasons
/// SpaceStationResult.None documents server-side: open-notify has never been
/// reached, the cached position is too stale to trust, or this device's own
/// position never resolved. There is no polar-style partial-absence case
/// here the way sunrise/sunset has - either all four live-position values
/// are real or none are - so this card reports zero items when both the live
/// position and the next pass are absent (see cardItemCount()) and drops out
/// of the rotation, the same tolerance MoonPhase.cpp's and Tides.cpp's own
/// cards get. The next-pass half has its own three parallel absence
/// reasons - see CheckIn.h's own issNextPassRiseUtc remarks - and additionally
/// stops counting as present once its own predicted set time has passed, so
/// this card never keeps showing a pass that has already happened just
/// because the device has not checked in again since.
///
/// **UNVERIFIED ON HARDWARE.** Like every other check-in-driven card in this
/// build, this firmware has no automated tests. What is here is checked by a
/// clean compile and by reading.
namespace IssFlyover {

/// This card's registered id, and the `id` a policy entry must use to
/// schedule it. Exposed so the id appears exactly once in the firmware.
/// Matches KnownCards.cs's "issflyover" on the server side exactly - a
/// mismatch here is what "policy names unknown card" in the log means. One
/// id for both display modes: a household's cardPolicy names this card once,
/// and which of the two it draws is this module's own runtime decision, not
/// something a policy needs to know about.
extern const char* const kCardId;

/// Called from App.ino on every successful check-in with the live-position
/// values off the response. distanceMiles negative means "nothing to show" -
/// see the header note above; a real distance is never negative, so it alone
/// is a safe absence gate for all four fields together. Cheap and
/// idempotent; safe to call on every check-in whether or not anything
/// changed.
void setPosition(double latitude, double longitude, double distanceMiles, double bearingDegrees);

/// Called from App.ino on every successful check-in with the next-pass
/// values off the response - CheckIn::Result's issNextPassRiseUtc and its
/// six siblings, plus the same utcOffsetMinutes Tides::setTimes() and
/// SunMoon::setTimes() already take, needed here to turn the absolute UTC
/// instants back into a local wall-clock string the same way ClockDate.cpp
/// does for the current time. riseUtc/setUtc of 0 means "no pass to show" -
/// see CheckIn.h's own remarks; a real instant is never exactly the Unix
/// epoch. Cheap and idempotent; safe to call on every check-in whether or
/// not anything changed.
void setNextPass(time_t riseUtc, double riseAzimuthDegrees, time_t maxElevationUtc,
                  double maxElevationDegrees, double maxElevationAzimuthDegrees, time_t setUtc,
                  double setAzimuthDegrees, int utcOffsetMinutes);

}  // namespace IssFlyover
