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

/// A single already-formatted line, with no trailing newline expected.
void line(const String& text);

/// A second, quieter logging tier for call sites that would otherwise add
/// real, ongoing cost to every device regardless of whether anyone is
/// watching - narrating exactly what a card currently has on screen from
/// inside its own draw function, say, which runs on every dwell cycle for as
/// long as that card stays in rotation. printf()/line() above always pay for
/// formatting and always write Serial, on purpose - Serial is this module's
/// fallback of last resort and must never regress (see this file's own
/// remarks at the top). verbose() does not carry that guarantee: it is a
/// complete no-op, formatting included, whenever streamingEnabled() is
/// false, which is every device's resting state and after every reboot. The
/// trade this makes on purpose is that a line logged only through verbose()
/// is invisible on a USB-Serial session with streaming off - acceptable here
/// because everything verbose() is for is detail a card's ordinary printf()/
/// line() calls already summarize on change, not the only record of
/// something happening.
void verbose(const char* format, ...) __attribute__((format(printf, 1, 2)));

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

}  // namespace Log
