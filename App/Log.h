#pragma once

#include <Arduino.h>

/// One shared place all of App's debug output goes through, so USB Serial and
/// the remote debug stream (see CheckIn.h's debugStreamRequested and the
/// POST /api/debuglog batches below) always narrate the identical story
/// rather than two logging paths silently drifting apart.
///
/// Named Log rather than something under App's existing per-concern module
/// names because every one of those modules calls into this - Display,
/// WifiJoin, CheckIn, Weather, AppUpdater, AppService and Loader all log
/// through here rather than each owning its own Serial calls.
///
/// Serial is written unconditionally, first, on every call, regardless of
/// streaming state or network reachability. If the remote stream is ever
/// broken, disabled, or the server is unreachable, someone with a USB cable
/// in hand must still see exactly what they would have seen before this
/// module existed - that is this module's fallback of last resort, not a
/// nice-to-have, and nothing about local debugging may regress because of it.
/// This holds for the heap cap below too: the pending buffer dropping a line
/// is invisible over the cable, because Serial was already written before the
/// buffer was ever consulted.
///
/// THE PENDING BUFFER YIELDS TO THE HEAP, and this is load-bearing rather
/// than a refinement. It is capped by line count and by bytes, and ALSO by
/// how much contiguous heap the device has left: while the largest free 8-bit
/// block is under two of Http::kTlsRecordBufferBytes - the figure CheckIn.cpp
/// already uses for "could not have held a fresh TLS session" - the buffer
/// holds one batch rather than its full ceiling, evicting oldest-first
/// exactly as it does at the byte cap.
///
/// Without that third cap the buffer is self-sustaining in the worst way: it
/// pins up to 16KB across 200 Arduino Strings, a new TLS session needs a
/// contiguous block the buffer has just eaten, and the POST that would DRAIN
/// the buffer is therefore the one request that cannot go out. Measured on
/// device 23 on 2026-09-27 as a 16,040-byte fall in 160 seconds ending in a
/// restart, repeating six times in 62 minutes on two devices. Log.cpp carries
/// the numbers and the argument for the threshold.
///
/// The consequence to know about as a caller: nothing changes about what you
/// may log or how often, but on a memory-starved device the stream is
/// SHALLOWER - roughly one batch of backlog instead of several minutes of it.
/// It says so itself when that happens, in a marker line composed directly
/// into the outgoing batch (it cannot be logged, for the obvious reason).
///
/// Buffering onto the remote stream only happens while the server's most
/// recent check-in response asked for it (CheckIn::Result::debugStreamRequested,
/// see setStreamingEnabled()). It is off by default and after every reboot,
/// and turns itself back on within one check-in interval if the server still
/// wants it - no NVS flag of its own to fall out of sync with the server's
/// actual, current state.
namespace Log {

/// printf-style logging, matching how call sites already format Serial
/// output (e.g. "[wifi] joined SSID=%s IP=%s RSSI=%d dBm channel=%d"). No
/// trailing newline is expected in format - one is always added. Formatted
/// output longer than fits a fixed 256-byte scratch buffer is kept and
/// marked "...(truncated)" rather than silently cut off mid-word or grown
/// with a heap allocation on every single log call.
void printf(const char* format, ...) __attribute__((format(printf, 1, 2)));

/// Like printf(), but a complete no-op - not even formatted - unless remote
/// debug streaming is currently on. Serial output for a fleet of devices
/// only one of which is being actively debugged would otherwise get far
/// noisier for no one watching it; this keeps that cost opt-in per device,
/// the same way the remote stream itself already is. Meant for the kind of
/// detail that is genuinely too much to want unconditionally - every server
/// request/response, exactly what a card drew this cycle - where printf()'s
/// existing "always on Serial" contract would be the wrong default rather
/// than merely a noisier one.
void verbose(const char* format, ...) __attribute__((format(printf, 1, 2)));

/// A single already-formatted line, with no trailing newline expected.
void line(const String& text);

/// The same, for a line that is already a plain C string - which is what
/// every literal call site and both formatting functions above actually
/// have.
///
/// **This overload exists to not allocate.** `line(const String&)` binding to
/// a `const char*` argument constructs a temporary Arduino `String`, which
/// heap-allocates and immediately frees 20-256 bytes. That happened on every
/// log line in the firmware whether or not anyone was listening: printf() and
/// verbose() both ended in `line(String(scratch))`, and this is the
/// highest-frequency allocation site in the whole image - every fetch, every
/// check-in field, every asset operation, and every card draw (they all pass
/// through CardManager's one draw choke point). Serial.println() takes a
/// `const char*` perfectly well, and the buffer copy only has to exist when
/// streaming is actually on, so on the ordinary path this now allocates
/// nothing at all.
///
/// Kept as an overload rather than replacing the String form: the other call
/// sites genuinely hold a String already, and forcing them through c_str()
/// would just move the same question somewhere less obvious.
void line(const char* text);

/// Called from performCheckIn() with the server's current
/// debugStreamRequested value on every successful check-in - unlike the
/// forced-update flag this is not one-shot, since streaming is meant to
/// track the server's toggle live and recover on its own after a reboot.
/// Turning streaming off makes one best-effort attempt to send whatever is
/// still buffered (see flushNow()) before the buffer is discarded, since a
/// device mid-update when an admin flips the toggle off is exactly the
/// moment those last lines matter most.
void setStreamingEnabled(bool enabled);

bool streamingEnabled();

/// Call once per loop() iteration. Sends a batch when the batch timer has
/// elapsed or the pending buffer has grown past a size worth sending early -
/// whichever comes first. A no-op whenever streaming is disabled.
void poll();

/// Forces an immediate, synchronous send of whatever is currently buffered,
/// bypassing the timer - a no-op when streaming is disabled. Used by
/// Loader.cpp right before every reboot path (requestUpdate(),
/// returnToLoaderForReprovisioning()), so the last lines explaining why
/// actually reach the server instead of being lost with everything else in
/// RAM at restart.
void flushNow();

/// Holds log uploads off the air while something more important is using it.
///
/// There are two memory budgets in this module, not one: the stored messages,
/// and the TLS session that drains them. Bounding the first does nothing about
/// the second, and the second is what matters here - a log POST and a check-in
/// POST each want a contiguous record buffer, and two of them alive at once is
/// a memory event on this board even where either alone would have been
/// affordable.
///
/// The log is the side that yields, always. A check-in carries the card policy,
/// the firmware manifest and the device's only means of being managed; a log
/// batch waits a second and loses nothing, because nothing is consumed from the
/// buffer until a send succeeds.
///
/// Wrap the check-in, not the whole loop. Suppressing for longer than the
/// request takes would starve the stream on a device that is checking in
/// frequently, which is the same device somebody most likely has the stream
/// open on. flushNow() deliberately ignores this - the pre-restart flush has no
/// later poll() to fall back on.
void setUploadsSuppressed(bool suppressed);

bool uploadsSuppressed();

/// Suppresses log uploads for the lifetime of the scope it is declared in.
///
/// A GUARD RATHER THAN PAIRED CALLS, and the reason is the shape of the code it
/// wraps. Check-in returns early on a 401, on any non-200, and on a parse
/// failure, and a release that one of those paths stepped over would leave the
/// stream suppressed forever - taking out the only diagnostic channel a
/// deployed device has, on the failure paths where somebody is most likely
/// watching it. A destructor cannot be stepped over.
///
/// Not re-entrant, deliberately: nesting two would release on the inner one's
/// destructor. There is one call site and it wraps one request.
class UploadSuppression {
 public:
  UploadSuppression() { setUploadsSuppressed(true); }
  ~UploadSuppression() { setUploadsSuppressed(false); }
  UploadSuppression(const UploadSuppression&) = delete;
  UploadSuppression& operator=(const UploadSuppression&) = delete;
};

}  // namespace Log
