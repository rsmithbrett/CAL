#pragma once

#include <Arduino.h>

/// Device health/diagnostics, reported to the server's Telemetry domain
/// (`POST /api/telemetry`) - see the DiscoverAroundMe repo's own README,
/// "Telemetry: device health/diagnostics", for the canonical wire contract.
/// This header only describes how the App populates that contract; the
/// field set, types, and response shape are settled there, not here.
///
/// That domain exists because of a real incident: a device's secret went
/// stale mid-run and it silently failed every check-in for hours with zero
/// server-side visibility, discovered only by hand-reading raw audit
/// records. Telemetry is the fix - a small, roughly-periodic heartbeat
/// (uptime, WiFi signal, free heap, CAL's own boot-attempt counter, SD
/// capacity/usage and cached-asset count, and this device's own last check-in
/// outcome) an admin's fleet page can watch for staleness. It is device
/// health/diagnostics, not product-usage analytics.
///
/// The three storage fields are here for the same reason free heap already
/// is. SD storage is treated as effectively unlimited - the card is
/// user-upgradeable - but that is only a defensible position while somebody
/// can see how full it is, so storage pressure shows up fleet-wide before it
/// shows up as a device that quietly stopped caching assets. All three read
/// zero on a device with no card in the slot, which is an ordinary supported
/// state rather than a fault (see SdStorage.h).
///
/// Deliberately has no timer of its own. It piggybacks on CheckIn's existing
/// cadence instead - see performCheckIn() in App.ino, which calls report()
/// once per successful check-in, immediately after. Telemetry data (signal
/// strength, free heap) changes slowly enough that it would be reasonable to
/// send it less often than every check-in, but the server's own
/// `/diag/telemetry` page flags a report as stale past three times the
/// check-in gateway's default interval - a threshold that is only a
/// meaningful signal if a healthy device's telemetry normally refreshes
/// close to every check-in, not on some slower, independent schedule that
/// would trip that alarm on its own. Riding check-in's cadence exactly
/// keeps that assumption true without this module needing to know what the
/// current interval even is.
namespace Telemetry {

/// Best-effort, fire-and-forget: a failed report is logged and dropped
/// rather than retried before the next check-in cycle comes around on its
/// own. Nothing downstream depends on this succeeding - see the README's own
/// remarks on why this is diagnostics, not a control channel.
///
/// lastCheckInOutcome must be exactly "ok", "updateAvailable", or
/// "secretRejected", matching CheckIn::Result's own outcomes verbatim (see
/// CheckIn.h) - the server stores this as the raw string a device sends
/// without parsing it into an enum of its own, but this firmware should
/// still only ever send one of the three values it can actually produce.
void report(const char* lastCheckInOutcome);

}  // namespace Telemetry
