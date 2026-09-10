#pragma once

#include <Arduino.h>

/// How much stack the loop task has actually had left, measured rather than
/// inferred.
///
/// **Why this exists, and why the obvious alternative is not good enough.**
/// The loop task's stack was raised from the core's 8192-byte default to 16384
/// after a live device crashed with "Stack canary watchpoint triggered
/// (loopTask)" - see getArduinoLoopTaskStackSize() in App.ino. Every judgement
/// about stack safety in this codebase since has been made by adding up the
/// sizes of local objects: CheckIn::Result is roughly 4KB, dominated by 28
/// PolicyEntry slots and their six Strings apiece; Forecast::fetch() holds two
/// JsonDocuments across a TLS handshake; and so on. That arithmetic is how the
/// announcement array came to be moved off the stack into a file-static buffer.
///
/// Adding up object sizes is a guess. It cannot see compiler-inserted
/// temporaries, spilled registers, inlining decisions, or how deep the call
/// chain actually goes inside mbedTLS or ArduinoJson - and it silently ignores
/// whichever frame happens to be the real peak. This firmware has already been
/// caught once this week reasoning confidently from a number that turned out
/// not to describe reality (ESP.getFreeHeap() and ESP.getMaxAllocHeap()
/// overstate free memory by roughly 4x on this board - see Display.cpp's
/// heapdiag). The lesson transfers exactly: measure the watermark, do not
/// infer the risk.
///
/// uxTaskGetStackHighWaterMark() reports the MINIMUM free stack that task has
/// ever had since it started - a true low-water mark, not an instantaneous
/// reading. That is precisely the number worth knowing, because the frame that
/// nearly overflowed is long gone by the time anyone asks.
namespace StackWatch {

/// Logs the loop task's minimum-ever remaining stack, tagged with `when`.
///
/// Call it either side of the paths suspected of being deep - the check-in
/// (a TLS handshake plus a JSON parse plus a ~4KB Result) and a card render -
/// so the log shows which one actually consumed the stack rather than which
/// one looked expensive on paper.
///
/// Reports the configured stack size alongside the remaining figure on
/// purpose. ESP-IDF returns this watermark in bytes where vanilla FreeRTOS
/// returns it in words, and rather than trust one reading of the docs, the log
/// prints both numbers so the ratio makes the unit self-evident: a
/// "remaining" value near 16384 is bytes, and one near 4096 for the same
/// device is words. Do not delete the second figure to tidy the line up - it
/// is what makes the first interpretable.
void logHighWaterMark(const char* when);

/// The raw watermark, for a caller that wants to compare two points itself
/// rather than read two log lines. Same units as above, whatever they prove
/// to be.
size_t highWaterMark();

}  // namespace StackWatch
