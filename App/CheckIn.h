#pragma once

#include <Arduino.h>

#include "Actions.h"
#include "Cards.h"

/// The device's regular heartbeat - what CheckInGatewayEndpoints/CheckInGatewayService
/// actually answer, distinct from both Weather's content fetch and AppUpdater's own
/// slower, independent manifest poll. This is the FAST path an admin's "Force update"
/// button (or a plain version bump) actually reaches: the server decides on every
/// check-in whether this device should update, and says so right there - no waiting
/// for AppUpdater's own hourly timer, which stays only as a belt-and-braces fallback.
namespace CheckIn {

struct Result {
  bool ok = false;
  bool acknowledged = false;
  bool updateAvailable = false;
  /// True only when the server answered 401 - this device's secret no longer
  /// authenticates (an admin's "Allow re-registration" or a secret regeneration
  /// while this device was mid-run, not a network problem). The App cannot
  /// re-enroll itself; the caller is expected to reboot into CAL via
  /// Loader::returnToLoaderForReprovisioning() rather than retry the same dead
  /// secret forever.
  bool secretRejected = false;
  /// Server-dictated cadence for the NEXT check-in - a household's fleet size is the
  /// server's decision to make, not a constant baked into every device's firmware.
  uint32_t intervalMs = 0;
  /// The server's current wish for remote debug-log streaming (see Log.h), reread
  /// on every check-in - unlike updateAvailable, this is NOT one-shot. It reflects
  /// whatever an admin's toggle says right now, so streaming turns on or off in
  /// step with the server rather than latching the first answer it ever saw, and
  /// naturally picks the right state back up after a reboot within one check-in
  /// interval, with no flag of its own to persist or fall out of sync.
  bool debugStreamRequested = false;
  /// One-shot, unlike debugStreamRequested above: true means an admin used
  /// DeviceRegistry's "Reformat SD card" button since this device's last
  /// check-in. The server clears the underlying flag the moment it answers
  /// with true (DeviceRegistryService.ConsumeSdReformatForcedAsync), the
  /// same one-shot-consume contract updateAvailable already has for a forced
  /// firmware update - so this is never true twice for one button press,
  /// even if App.ino only checks it once per check-in. Absent on a server
  /// that predates this field, which JsonDocument's `| false` default
  /// already handles the same way every other additive field here does.
  bool sdReformatRequested = false;
  /// Minutes to add to UTC to get this device's local time right now - DST
  /// already applied, recomputed by the server fresh on every check-in from
  /// the device's own location rather than looked up once and cached. Local
  /// time is just `time(nullptr) + utcOffsetMinutes * 60` (see Display.cpp's
  /// drawClock()). Defaults to 0 (UTC) on a struct nothing has populated yet -
  /// see App.ino's own lastUtcOffsetMinutes, which persists this across the
  /// gaps between check-ins the same way checkInIntervalMs already does, and
  /// Identity::lastUtcOffsetMinutes(), which mirrors it to NVS so it also
  /// survives a reboot that happens before this run's first check-in ever
  /// completes.
  int utcOffsetMinutes = 0;
  /// Whether the Sun is up right now at the device's location, recomputed
  /// fresh from real sunrise/sunset on every check-in - not a fixed
  /// day/night schedule. Defaults to true (daytime), matching the server's
  /// own fallback for an unresolved location (DeviceLocalTimeResult.Fallback
  /// in the DiscoverAroundMe repo).
  bool isDaytime = true;

  /// Today's sunrise and sunset as minutes after UTC midnight (0-1439), or -1
  /// where the server reported none. Minutes rather than timestamps because
  /// `utcOffsetMinutes` above is already in the same unit, so local wall-clock
  /// is `(value + utcOffsetMinutes + 1440) % 1440` with no date arithmetic and
  /// no 64-bit epoch handling.
  ///
  /// -1 rather than 0 for "absent": 0 is a real time (UTC midnight). The server
  /// sends JSON null in three cases that are all genuinely "there is no answer"
  /// rather than an error - polar day, polar night, and a device whose position
  /// has never resolved. A card is expected to say the Sun does not rise or set
  /// rather than render a placeholder.
  int sunriseMinutesUtc = -1;
  int sunsetMinutesUtc = -1;

  /// The Moon's elongation from the Sun as a fraction of the cycle (0 new,
  /// 0.25 first quarter, 0.5 full, 0.75 last quarter) and the fraction of
  /// its visible disc lit (0-1), or -1 for either where the server has no
  /// answer. Unlike sunrise/sunset there is no polar-style "does not happen
  /// today" case for these - the phase is a fact of the date, not of the
  /// observer's horizon - so -1 means only one thing: a device whose
  /// position has never resolved (see DeviceLocalTimeResult in the
  /// DiscoverAroundMe repo). moonPhaseName is empty under the same
  /// condition - a human label for moonPhase ("Waxing Gibbous"), carried
  /// alongside the two numbers as a fallback caption for the card that
  /// draws them as a picture.
  double moonPhase = -1.0;
  double moonIlluminatedFraction = -1.0;
  String moonPhaseName;

  /// The next high and low tide as minutes after UTC midnight (0-1439), or -1
  /// where the server reported none - same unit and the same "-1 rather than
  /// 0" reasoning as sunriseMinutesUtc/sunsetMinutesUtc above. Backed by real
  /// NOAA CO-OPS data on the server; absent means no NOAA station within
  /// range of the resolved position, no position resolved at all, or NOAA
  /// unreachable with nothing cached - all genuinely "there is no answer"
  /// rather than an error, the same three-reasons-one-treatment shape
  /// sunrise/sunset already has.
  int nextHighTideMinutesUtc = -1;
  int nextLowTideMinutesUtc = -1;

  /// The International Space Station's current sub-satellite point, and the
  /// distance/bearing from this device's resolved position to it - straight
  /// off the check-in response, same as every field above. issDistanceMiles
  /// negative means "nothing to report" (open-notify unreachable, the cached
  /// position too stale to trust, or this device's position never
  /// resolved) - see CheckInModels.cs's own IssLatitude remarks for the full
  /// list of reasons, all genuinely "there is no answer" rather than an
  /// error. issLatitude/issLongitude default outside any real coordinate
  /// rather than to 0, so an accidental read before the first check-in reads
  /// as obviously wrong rather than as the Gulf of Guinea.
  double issLatitude = -999.0;
  double issLongitude = -999.0;
  double issDistanceMiles = -1.0;
  double issBearingDegrees = -1.0;

  /// The station's next predicted pass above this device's own horizon - a
  /// completely different feature from issLatitude/issDistanceMiles above,
  /// which are the live sub-satellite point right now. See CheckInModels.cs's
  /// own IssNextPassRiseUtc remarks: this is real CelesTrak/SGP4 orbital
  /// mechanics, a pass can be several days out, and it was pushed to the
  /// check-in response long before this firmware ever read it - "found live"
  /// while chasing the household's "space station is not reporting when it
  /// will fly over" report, see IssFlyover.h.
  ///
  /// The three *Utc fields are absolute UTC instants (epoch seconds), unlike
  /// sunriseMinutesUtc/nextHighTideMinutesUtc's minutes-into-today above -
  /// parsed off the server's ISO-8601 strings by CheckIn.cpp's own
  /// parseIso8601Utc(), since a pass can land on a different calendar day
  /// than the check-in that reported it. 0 rather than -1 as the "absent"
  /// sentinel: an epoch of 0 is 1970-01-01, as obviously wrong for a future
  /// pass as issLatitude's -999.0 is for a real coordinate, and a real
  /// instant is never exactly the Unix epoch. Absent for the same three
  /// reasons issLatitude is: no CelesTrak element set ever fetched, this
  /// device's own position never resolved, or no qualifying pass found in
  /// the server's search window - all genuinely "there is no answer" rather
  /// than an error.
  ///
  /// The four *AzimuthDegrees/the elevation field are compass/geometric
  /// facts (0-360 and 0-90 respectively), so -1.0 is the "absent" sentinel
  /// for them instead, matching issBearingDegrees's own convention - a real
  /// azimuth or elevation is never negative.
  time_t issNextPassRiseUtc = 0;
  double issNextPassRiseAzimuthDegrees = -1.0;
  time_t issNextPassMaxElevationUtc = 0;
  double issNextPassMaxElevationDegrees = -1.0;
  double issNextPassMaxElevationAzimuthDegrees = -1.0;
  time_t issNextPassSetUtc = 0;
  double issNextPassSetAzimuthDegrees = -1.0;

  /// The household's home value, as an automated valuation model (AVM)
  /// estimate from RentCast - pushed on every check-in the same way every
  /// other check-in-driven card's data is, see App/HomeValue.h's own
  /// remarks. homeValueEstimate is the headline dollar figure;
  /// homeValueRangeLow/High are the AVM's own confidence range;
  /// homeValuePricePerSquareFoot is that same estimate divided out per
  /// square foot. -1/-1.0 for whichever the server has no answer for - the
  /// device has no owner or no usable home address, RentCast was never
  /// reached, or the device was rejected before this logic ever ran, all
  /// genuinely "there is no answer" rather than an error, the same
  /// three-reasons-one-treatment shape sunrise/sunset and the tide fields
  /// above already have. A real dollar figure or per-square-foot price is
  /// never negative, so -1/-1.0 is a safe, independent absence sentinel for
  /// each of the four.
  ///
  /// homeValueUpdatedAtUtc is when RentCast was last actually asked, not when
  /// this check-in happened - parsed off the server's ISO-8601 string by
  /// CheckIn.cpp's own parseIso8601Utc(), the same helper issNextPassRiseUtc
  /// above uses and for the same reason. 0 is the "absent" sentinel,
  /// matching that field's own convention: a real instant is never exactly
  /// the Unix epoch.
  int homeValueEstimate = -1;
  int homeValueRangeLow = -1;
  int homeValueRangeHigh = -1;
  double homeValuePricePerSquareFoot = -1.0;
  time_t homeValueUpdatedAtUtc = 0;

  /// How this device should rotate its cards. `present == false` means the
  /// server sent no policy this time, which means "keep whatever policy you
  /// already had" - explicitly not "show nothing". See
  /// CardManager::applyPolicy().
  Cards::Policy cardPolicy;

  /// The buttons this device's cards should draw, resolved server-side from
  /// the account's action bindings. An empty set is completely normal and
  /// means no card draws any buttons. Re-sent on every check-in, so this is
  /// not one-shot: the device simply matches its button set to whatever the
  /// latest response said, the same "always current" contract
  /// debugStreamRequested has.
  Actions::Definition cardActions[Actions::kMaxDefinitions];
  uint8_t cardActionCount = 0;

  /// Which of the pendingActions this request carried the server has now
  /// recorded, and which the device may therefore stop carrying. Pure dedup
  /// bookkeeping - NOT confirmation for whoever pressed the button, who by
  /// design gets none and waits for nothing. See Actions.h.
  String acceptedActionIds[Actions::kMaxPending];
  uint8_t acceptedActionCount = 0;
};

/// This board has no battery (ELEGOO/CYD is USB-powered) - batteryPercent/charging are
/// sent as fixed placeholders rather than omitted, since CheckInRequest has no way to
/// say "not applicable" and a battery-powered sibling board will want the real fields
/// this same call already sends.
Result perform();

}  // namespace CheckIn
