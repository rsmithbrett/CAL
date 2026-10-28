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
/// THREE heap fields are sent, not one, and that is not redundancy.
/// `freeHeapBytes` is ESP.getFreeHeap(), which was measured overstating the
/// memory an allocation can actually reach by roughly 4x on this board (49,960
/// against a real 11,340 at the same instant on device 17, while a 10,568-byte
/// malloc was failing). `free8BitBytes` and `largestFreeBlock8BitBytes` are
/// heap_caps_get_free_size/heap_caps_get_largest_free_block against
/// MALLOC_CAP_8BIT - the pool a byte buffer genuinely comes from. The old field
/// keeps its old meaning because the server must keep working with firmware up
/// to six months old and because two columns side by side are what make the
/// discrepancy checkable in the fleet's own history; see report()'s own
/// comments for the full argument. The largest block is reported as well as the
/// free size because a free total alone cannot tell "plenty of room" from
/// "plenty of room in unusable fragments", which is precisely the state that
/// went unnoticed.
///
/// **And three more beside them, over internal 8-bit heap:**
/// `free8BitInternalBytes`, `largestFreeBlock8BitInternalBytes` and
/// `bootLargestFreeBlock8BitInternalBytes`, plus `heapPoolsMeasured` naming what
/// the bare fields span (`"internal"` or `"internal+psram"`). The bare three are
/// `MALLOC_CAP_8BIT`, which is a permission and not a pool: on a board with
/// PSRAM registered in its heap they describe internal DRAM and PSRAM together,
/// and mbedTLS reaches only the first half of that - it allocates
/// `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`. So the internal three are the ones a
/// TLS floor is graded against. Added rather than substituted, for the reason
/// `freeHeapBytes` was: a field does not change meaning underneath a server that
/// reads it. On every device in the fleet the internal three repeat the bare
/// three exactly, since an ESP32-D0WD-V3 registers no PSRAM. Absent or null
/// means what a report from older firmware already meant - one pool, and no
/// distinction to draw. See HeapPools.h.
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
