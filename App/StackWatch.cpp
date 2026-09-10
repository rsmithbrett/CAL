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

  // Log::verbose, not printf: this is investigation instrumentation on a path
  // that runs every loop iteration, and it costs a device with streaming off
  // nothing at all (see Log.h's own remarks on the const char* overload and
  // the early return). A device someone is actually watching gets the full
  // trace.
  Log::verbose("[stack] %s | loopTask min-free=%u (worst-ever=%u) of %u configured", when,
               static_cast<unsigned>(remaining), static_cast<unsigned>(gWorstSeen),
               static_cast<unsigned>(kConfiguredLoopStackBytes));
}

}  // namespace StackWatch
