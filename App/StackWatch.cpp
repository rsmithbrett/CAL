#include "StackWatch.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "Log.h"

namespace StackWatch {
namespace {

/// What getArduinoLoopTaskStackSize() in App.ino returns. Duplicated as a
/// display-only figure rather than shared, because that function deliberately
/// lives at global scope outside any namespace so the linker can override the
/// core's weak symbol - reaching for it from here would drag that constraint
/// into a second file for no benefit. It is only ever printed, never used to
/// decide anything, so a drift between the two costs a confusing log line and
/// nothing more.
constexpr size_t kConfiguredLoopStackBytes = 16384;

/// The lowest watermark seen so far in this process, so a single line can say
/// both "right now" and "worst ever" - the worst-ever figure is what actually
/// matters and it is easy to miss if it happened between two log calls.
size_t gWorstSeen = SIZE_MAX;

/// What was last actually printed, and when. Both exist to stop this from
/// flooding the log, which it did: a real capture from device 17 carried
/// several hundred consecutive identical "[stack] after render | min-free=6812"
/// lines, because the after-render call site runs on every loop iteration and
/// this logged unconditionally. That is worse than useless - it buries the
/// [heapdiag] and [checkin] lines someone is actually reading, and on a device
/// with streaming on it spends the remote channel, which is the only
/// diagnostic channel a deployed device has, on saying nothing changed.
///
/// Note what makes "log only on change" sufficient here rather than lossy:
/// uxTaskGetStackHighWaterMark returns the MINIMUM free the task has ever
/// seen, so it is monotonically non-increasing for the life of the task. It can
/// only ever move one way. Suppressing repeats therefore hides nothing - every
/// distinct value this figure will ever take still gets printed exactly once,
/// at the moment it first happens.
size_t gLastLogged = SIZE_MAX;
uint32_t gLastLogMs = 0;

/// A heartbeat so a steady figure still proves the instrumentation is alive.
/// Without it, "the stack has been fine for an hour" and "this stopped being
/// called an hour ago" produce identical logs, and those are not the same fact.
constexpr uint32_t kHeartbeatMs = 300000;  // 5 minutes

}  // namespace

size_t highWaterMark() { return uxTaskGetStackHighWaterMark(nullptr); }

namespace {

bool gUnitsReported = false;

/// Settles the bytes-versus-words question by proof rather than by reading the
/// documentation, and says so out loud once.
///
/// The unit this call returns is implementation-specific in practice: ESP-IDF
/// is documented as returning bytes while vanilla FreeRTOS returns stack words,
/// and a build can end up either way. Converting a figure whose unit is
/// uncertain is exactly how this codebase already burned a night on
/// ESP.getMaxAllocHeap(), so this does not convert anything.
///
/// The test is one-way and sound. The loop task's stack is
/// kConfiguredLoopStackBytes; a word is 4 bytes on this chip; so a word-based
/// reading can never exceed kConfiguredLoopStackBytes / 4. **Any reading above
/// that quarter therefore proves the figure is in bytes.** A reading at or
/// below it proves nothing either way - it could be words, or it could be
/// bytes on a build whose setup() went deep - and this says that rather than
/// guessing, because a confident wrong answer here is worse than an
/// acknowledged unknown.
void reportUnitsOnce(size_t remaining) {
  if (gUnitsReported) {
    return;
  }
  gUnitsReported = true;

  constexpr size_t kMaxIfWords = kConfiguredLoopStackBytes / 4;
  if (remaining > kMaxIfWords) {
    Log::printf(
        "[stack] unit resolved: min-free=%u exceeds %u, the largest a word count could be for a "
        "%u-byte stack, so this figure is BYTES",
        static_cast<unsigned>(remaining), static_cast<unsigned>(kMaxIfWords),
        static_cast<unsigned>(kConfiguredLoopStackBytes));
  } else {
    Log::printf(
        "[stack] unit UNRESOLVED: min-free=%u is at or below %u, which is consistent with both "
        "words and a deep-setup byte count - treat the figure as a relative trend, not an "
        "absolute, until a higher reading settles it",
        static_cast<unsigned>(remaining), static_cast<unsigned>(kMaxIfWords));
  }
}

}  // namespace

void logHighWaterMark(const char* when) {
  const size_t remaining = highWaterMark();
  if (remaining < gWorstSeen) {
    gWorstSeen = remaining;
  }

  reportUnitsOnce(remaining);

  // Silence when nothing has changed. The after-render call site runs on every
  // loop iteration, so logging unconditionally produced hundreds of identical
  // consecutive lines on a real device - see gLastLogged's own remarks for why
  // suppressing them loses no information at all, given this figure can only
  // ever move one way.
  const uint32_t now = millis();
  const bool changed = (remaining != gLastLogged);
  // Subtraction, not `now > gLastLogMs + kHeartbeatMs`: millis() wraps at ~49
  // days and this firmware is meant to run for months. Unsigned wrap-around
  // makes the difference correct across the rollover; the comparison form is
  // not, and would go quiet for 49 days the first time it happened.
  const bool heartbeatDue = (now - gLastLogMs) >= kHeartbeatMs;

  if (!changed && !heartbeatDue) {
    return;
  }

  gLastLogged = remaining;
  gLastLogMs = now;

  // Log::verbose, not printf: this is investigation instrumentation, and it
  // costs a device with streaming off nothing at all (see Log.h's own remarks
  // on the const char* overload and the early return). A device someone is
  // actually watching gets the full trace.
  Log::verbose("[stack] %s | loopTask min-free=%u (worst-ever=%u) of %u configured%s", when,
               static_cast<unsigned>(remaining), static_cast<unsigned>(gWorstSeen),
               static_cast<unsigned>(kConfiguredLoopStackBytes),
               changed ? "" : " (unchanged - heartbeat)");
}

}  // namespace StackWatch
