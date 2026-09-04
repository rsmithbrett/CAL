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
  /// Minutes to add to UTC to get this device's local time right now - DST
  /// already applied, recomputed by the server fresh on every check-in from
  /// the device's own location rather than looked up once and cached. Local
  /// time is just `time(nullptr) + utcOffsetMinutes * 60` (see Display.cpp's
  /// drawClock()). Defaults to 0 (UTC) until the first successful check-in -
  /// see App.ino's own lastUtcOffsetMinutes, which persists this across the
  /// gaps between check-ins the same way checkInIntervalMs already does.
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
