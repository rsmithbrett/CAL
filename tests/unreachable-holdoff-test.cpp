// App/UnreachableHoldOff.h: whether a run of failed check-ins points at this
// device or at the far end.
//
// The case this exists for was measured on 2026-11-07. Seven of twelve displays
// reported SOFTWARE_RESET+UNREACHABLE as their last restart reason, on a fleet
// whose wall units were running 25 to 47 hours between boots. A restart cannot
// reach a server that is down, and card policy does not survive one, so every
// one of those restarts turned a display full of content into "Nothing to show
// yet" for as long as the outage lasted.
//
// The header is compiled as it ships and names only <stdint.h>, so there are no
// stubs. Every case is asserted twice, as a static_assert over the constexpr
// function and again at runtime, so a widened predicate stops the compile on
// any toolchain and also prints a Failed! line under a runner.

#include <cassert>
#include <cstdio>

#include "UnreachableHoldOff.h"

namespace {

using UnreachableHoldOff::Decision;
using UnreachableHoldOff::decide;

// kMaxConsecutiveCheckInFailures and kMaxMsUnreachableWithHealthyHeap in
// App.ino. Repeated rather than included because App.ino is a sketch.
constexpr uint32_t kThreshold = 5;
constexpr uint32_t kDeadlineMs = 60UL * 60UL * 1000UL;

constexpr Decision healthyHeap(uint32_t failures, uint32_t forMs) {
  return decide(false, failures, kThreshold, forMs, kDeadlineMs);
}

constexpr Decision cannotReconnect(uint32_t failures, uint32_t forMs) {
  return decide(true, failures, kThreshold, forMs, kDeadlineMs);
}

// --- What must not change ------------------------------------------------
//
// The watchdog exists for devices 12 and 17, measured 2026-09-11: 9,204 and
// 4,084 bytes contiguous against a 16,717 requirement, drawing their cards
// perfectly and invisible to the server for hours. A restart recovered that
// every time and nothing else did.

static_assert(cannotReconnect(1, 0) == Decision::Restart,
              "A device that provably cannot build a session restarts on the first failure.");
static_assert(cannotReconnect(1, kDeadlineMs * 10) == Decision::Restart,
              "The proof does not wait on any clock.");
static_assert(cannotReconnect(0, 0) == Decision::HoldOff,
              "The proof alone is not enough: a device below the floor whose session is still "
              "up has nothing wrong with it.");
static_assert(healthyHeap(0, kDeadlineMs * 10) == Decision::HoldOff,
              "No failures, nothing to act on, however long the device has been up.");
static_assert(healthyHeap(kThreshold - 1, kDeadlineMs * 10) == Decision::HoldOff,
              "Under the threshold is still under the threshold.");

// --- The new rule --------------------------------------------------------
//
// Ample contiguous heap and the requests failing anyway is the ambiguous case,
// and the ambiguity is the point: a server restart, a router blip, DNS, or an
// outage that no restart of this device can shorten. The device holds its
// cards until the far end has had a fair chance to come back.

static_assert(healthyHeap(kThreshold, 0) == Decision::HoldOff,
              "Five failures on a device that could open a session says nothing about this "
              "device. This is the case that blanked the fleet.");
static_assert(healthyHeap(kThreshold, kDeadlineMs - 1) == Decision::HoldOff,
              "Still inside the deadline.");
static_assert(healthyHeap(kThreshold * 100, kDeadlineMs - 1) == Decision::HoldOff,
              "The count does not shorten the deadline - a longer outage is still an outage.");
static_assert(healthyHeap(kThreshold, kDeadlineMs) == Decision::Restart,
              "Past the deadline it is a last resort again: an hour of silence with room to "
              "reconnect is no longer obviously the far end's fault.");
static_assert(healthyHeap(kThreshold - 1, kDeadlineMs * 10) == Decision::HoldOff,
              "The deadline does not replace the threshold. Both still have to be met.");

struct Case {
  bool provablyCannotReconnect;
  uint32_t failures;
  uint32_t forMs;
  Decision expected;
  const char* what;
};

constexpr Case kCases[] = {
    {true, 1, 0, Decision::Restart, "proof, first failure"},
    {true, 1, kDeadlineMs * 10, Decision::Restart, "proof ignores the clock"},
    {true, 0, 0, Decision::HoldOff, "proof without a failure"},
    {false, 0, kDeadlineMs * 10, Decision::HoldOff, "no failures"},
    {false, kThreshold - 1, kDeadlineMs * 10, Decision::HoldOff, "under the threshold"},
    {false, kThreshold, 0, Decision::HoldOff, "threshold met, deadline not"},
    {false, kThreshold, kDeadlineMs - 1, Decision::HoldOff, "one millisecond short"},
    {false, kThreshold * 100, kDeadlineMs - 1, Decision::HoldOff, "many failures, still short"},
    {false, kThreshold, kDeadlineMs, Decision::Restart, "both met"},
};

}  // namespace

int main() {
  for (const auto& c : kCases) {
    const Decision got =
        decide(c.provablyCannotReconnect, c.failures, kThreshold, c.forMs, kDeadlineMs);

    if (got != c.expected) {
      std::printf("Failed: %s - expected %s, got %s\n", c.what,
                  c.expected == Decision::Restart ? "Restart" : "HoldOff",
                  got == Decision::Restart ? "Restart" : "HoldOff");
      return 1;
    }
  }

  std::printf("PASS: unreachable hold-off, %zu cases\n",
              sizeof(kCases) / sizeof(kCases[0]));
  return 0;
}
