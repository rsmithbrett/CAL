#pragma once

#include <Arduino.h>

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
};

/// This board has no battery (ELEGOO/CYD is USB-powered) - batteryPercent/charging are
/// sent as fixed placeholders rather than omitted, since CheckInRequest has no way to
/// say "not applicable" and a battery-powered sibling board will want the real fields
/// this same call already sends.
Result perform();

}  // namespace CheckIn
