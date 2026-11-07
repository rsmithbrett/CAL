#pragma once

// Whether a run of failed check-ins is this device's fault, and therefore
// whether restarting could fix it.
//
// Pulled out of checkUnreachableWatchdog() in App.ino so the decision can be
// walked on a host with no Arduino, no network and no clock. Only <stdint.h>,
// for the reason App/ReportedCounter.h gives: a header a test can include as it
// ships needs no stubs.
//
// The watchdog itself still owns everything around this - the maintenance
// window, the WiFi branch, the backoff floor, the logging and the restart. This
// answers one question: given that the device is associated, outside a window
// and past its backoff, does the evidence point at this device or at the far
// end?

#include <stdint.h>

namespace UnreachableHoldOff {

/// What the watchdog should do with the facts it has.
enum class Decision {
  /// Leave the device alone. Its cards keep drawing whatever it last received.
  HoldOff,

  /// Restart. Only ever returned when a restart could plausibly end the
  /// condition.
  Restart,
};

/// \param provablyCannotReconnect This device's largest contiguous internal
///        block is smaller than one mbedTLS record buffer, so mbedtls_ssl_setup()
///        cannot succeed - a local fault, proven locally, that a restart clears.
/// \param consecutiveFailures Check-ins that have failed in a row.
/// \param failureThreshold kMaxConsecutiveCheckInFailures.
/// \param unreachableForMs How long this episode has lasted, from the first
///        failure of the run.
/// \param healthyHeapDeadlineMs How long a device with room to build a session
///        waits before restarting anyway.
constexpr Decision decide(bool provablyCannotReconnect,
                          uint32_t consecutiveFailures,
                          uint32_t failureThreshold,
                          uint32_t unreachableForMs,
                          uint32_t healthyHeapDeadlineMs) {
  if (provablyCannotReconnect && consecutiveFailures > 0) {
    return Decision::Restart;
  }

  if (consecutiveFailures < failureThreshold) {
    return Decision::HoldOff;
  }

  // AMPLE CONTIGUOUS HEAP AND THE REQUESTS FAILING ANYWAY.
  //
  // This is the ambiguous half the count was kept for - a server restart, a
  // router blip, DNS - and until 2026-11-07 it restarted after five failures
  // the same as any other. On 2026-11-07 seven of twelve displays reported
  // SOFTWARE_RESET+UNREACHABLE as their last restart reason, on a fleet whose
  // wall units run 25 to 47 hours between boots. Card policy lives in RAM, so
  // each of those restarts replaced a drawing display with "Nothing to show
  // yet" until the server came back - and the restart could not have brought it
  // back, because the far end is what was down.
  //
  // So the device waits. It keeps its cards, keeps counting, and takes the
  // restart only if an hour passes with room to build a session and still no
  // answer. An outage shorter than that now costs nothing at all.
  //
  // The restart stays available rather than being removed, because an hour of
  // silence from a device that demonstrably had the heap to reconnect is no
  // longer obviously the far end's fault, and a wedged client with room to
  // spare has no other recovery. What changes is which way the doubt falls.
  //
  // The deadline is wall clock, not a count, and the two are not
  // interchangeable: the count climbs with the check-in cadence, which the
  // server sets, so a device told to check in every ten seconds would reach any
  // threshold in a fraction of the time an outage lasts.
  if (unreachableForMs < healthyHeapDeadlineMs) {
    return Decision::HoldOff;
  }

  return Decision::Restart;
}

}  // namespace UnreachableHoldOff
