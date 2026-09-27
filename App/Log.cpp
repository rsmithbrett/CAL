#include "Log.h"

#include <ArduinoJson.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "Config.h"
#include "Http.h"
#include "Identity.h"

namespace Log {
namespace {

constexpr const char* kPath = "/api/debuglog";

// The pending-buffer ceiling: this is what actually bounds memory if the
// network is down for a while, independent of (and larger than) the
// per-POST batch cap below. Once either limit here is hit, the oldest
// buffered line is dropped to make room for the newest, rather than growing
// forever or risking a failed allocation on a board this RAM-constrained.
// 200 lines / 16KB is generous headroom for several minutes of this
// firmware's actual log volume (roughly one line every few seconds in
// normal operation, brief bursts during WiFi join/check-in/update) while
// still being a small, fixed slice of the ESP32's ~320KB SRAM - and it is
// only ever paid while an admin has actually turned streaming on.
constexpr size_t kMaxBufferedLines = 200;
constexpr size_t kMaxBufferedBytes = 16384;

// THE THIRD CAP, AND THE ONLY ONE THAT LOOKS OUTSIDE THIS MODULE.
//
// The two caps above bound the buffer in lines and in bytes and say nothing
// at all about whether the device can afford either. On 2026-09-27 that
// turned out to be the whole of a field defect. Device 23 ("Test2"), with
// streaming on, per boot:
//
//     uptime   free8 total   largest free block
//       12 s        31,100               18,420
//      104 s        19,480                8,692
//      160 s        15,060                4,596   -> restart
//
// A 16,040-byte fall in 160 seconds against a kMaxBufferedBytes of 16,384,
// and the largest block's share of the total falling from 59% to 35% - which
// is the 200 separate String allocations fragmenting, not one 16KB block
// being held. HeapRatchet.h already names this module "the dominant
// consumer (200 String slots, a 16KB ceiling, ...)" on exactly those grounds;
// this is that sentence turning into a restart loop.
//
// WHY IT CANNOT RECOVER ON ITS OWN, which is what makes it a defect rather
// than a known cost. Draining this buffer means POSTing it, a POST needs a
// TLS session, and a new TLS session needs a contiguous
// Http::kTlsRecordBufferBytes that the buffer has just eaten. The buffer pins
// the memory required to empty the buffer. Check-ins fail for the same
// reason, App.ino's unreachable watchdog restarts the device, streaming comes
// back on at the next check-in, and it happens again - devices 17 and 23
// booted six times in 62 minutes this way and never got out of it. Devices
// 12, 18 and 30 have streaming off and are healthy.
//
// So the cap has to yield to the heap, and it has to yield BEFORE the floor
// rather than at it.
//
// WHY NOT Http::canOpenNewSession() DIRECTLY, since it is the obvious
// candidate. Two reasons, both of them stated in its own header. It is
// documented as a PROOF OF IMPOSSIBILITY - the line past which recovery is
// already gone, a fact to act on by restarting - and a proof of impossibility
// is not a budget to be spent down to. By the time it reads false the device
// is already in the state this change exists to prevent. And retreating from
// that line does not undo it: freeing 200 scattered Strings gives back bytes
// without necessarily giving back one contiguous 16,717-byte block, so a
// buffer that waits for the floor and only then evicts may not buy back the
// thing it gave away. The buffer has to stay clear of the floor, not fall
// back from it, so what is wanted here is headroom derived from
// kTlsRecordBufferBytes rather than the predicate itself.
//
// THE HEADROOM FIGURE, WHICH IS NOT A NEW ONE. A FRESH session needs
// Http::kTlsRecordBufferBytes twice - 16,717 x 2 = 33,434 - in two flat
// mbedtls_calloc() calls. CheckIn.cpp already tests exactly
// `largest < 2 * Http::kTlsRecordBufferBytes` and calls it "could not have
// held a fresh TLS session", argued there at length and confirmed on device
// 17 across 74 minutes of stream. This module is asking the identical
// question - can this device still get a session if it loses the one it has -
// so it deliberately asks it with the identical number rather than inventing
// a second threshold that could drift away from the first.
//
// It is a conservative reading of that requirement, and knowingly so: the two
// buffers need not come from one block, so a device below 33,434 contiguous
// may still handshake fine. Conservative is the correct direction here
// because the two errors are not symmetrical. Being wrong this way costs a
// shallower log buffer on a device that is low on memory. Being wrong the
// other way costs the device, which is what the table above is a picture of.
constexpr size_t kMinLargestBlockBytes = 2 * Http::kTlsRecordBufferBytes;

// The per-POST batch ceiling: deliberately smaller than the buffer above, so
// a single flush sends a reasonably sized request instead of dumping the
// entire pending buffer into one POST body (which would spike that
// request's latency and hold up the loop() driving it for longer than
// necessary). Whatever doesn't fit in one batch simply waits for the next
// poll() - the buffer cap above, not this one, is what bounds total memory.
//
// These are also what the buffer shrinks to when kMinLargestBlockBytes above
// says the heap cannot carry the full ceiling, and that reuse is deliberate
// rather than convenient.
//
// THE LOW-HEAP CEILING IS ONE BATCH, NOT NOTHING. A device short of heap is
// precisely the device somebody has a stream open on, and a buffer that
// refuses to hold anything would answer a memory problem by destroying the
// only diagnostic channel a deployed device has - handing the reader a
// silence that looks exactly like a healthy quiet device. One batch is the
// smallest size at which poll() still has a full POST to hand over, so the
// stream keeps running at its ordinary rate and gives up only its
// depth-on-outage: the several-minutes-of-backlog property, which is the
// part that was never affordable on this board in the first place.
//
// Expressed as these constants rather than as two new numbers because "as
// much as one POST can carry" is exactly the quantity wanted, it is already
// argued directly above, and tying them together makes it impossible for the
// low-heap ceiling to end up SMALLER than a single batch - which would
// strand lines that no flush could ever pick up.
constexpr size_t kMaxBatchLines = 40;
constexpr size_t kMaxBatchBytes = 4096;

// How often poll() actually sends, measured against millis() the same way
// every other timer in this codebase already works (checkInIntervalMs, the
// card manager's own per-card refresh timer, ...). This stays at 1000ms even
// though App's loop() now spins roughly twenty times faster than that: the
// closing delay() came down from 1000ms to 50ms so touch is sampled often
// enough to catch a real tap (see App.ino), which means poll() is now called
// far more often than it needs to send. The timer below is what keeps that
// from turning into twenty POSTs a second; it is no longer merely
// aspirational, as it was while loop() itself was the coarser limit. The
// line/byte caps above still catch a sudden burst faster than this timer
// would.
constexpr uint32_t kBatchIntervalMs = 1000;

bool streaming = false;
String buffer[kMaxBufferedLines];
size_t head = 0;
size_t count = 0;
size_t bufferedBytes = 0;
uint32_t droppedSinceFlush = 0;

// The subset of droppedSinceFlush that went for heap reasons rather than for
// hitting the line or byte ceiling, and the measurement that caused the most
// recent one. Kept apart from the total because the two say different things
// to whoever is reading the stream: the ordinary drop means the device is
// logging faster than it can send, and this one means the device is short of
// the memory it needs to send at all. Only the second predicts a restart, and
// a single combined number could not tell them apart.
uint32_t droppedForHeapSinceFlush = 0;
size_t largestBlockAtLastHeapDrop = 0;

uint32_t lastFlushMs = 0;

void clearBuffer() {
  for (size_t i = 0; i < kMaxBufferedLines; ++i) {
    buffer[i] = String();
  }
  head = 0;
  count = 0;
  bufferedBytes = 0;
  droppedSinceFlush = 0;
  droppedForHeapSinceFlush = 0;
  largestBlockAtLastHeapDrop = 0;
}

void pushToBuffer(const String& text) {
  const size_t textCost = text.length() + 1;  // +1 for the newline joining it to the next line

  // The ceilings in force for THIS push. Normally the line/byte caps; one
  // batch's worth when the heap says the full ceiling is not affordable (see
  // kMinLargestBlockBytes).
  size_t lineCeiling = kMaxBufferedLines;
  size_t byteCeiling = kMaxBufferedBytes;
  bool heapConstrained = false;
  size_t largestBlock = 0;

  // The heap is consulted only once this push would take the buffer past what
  // a single POST carries, and not before. Two reasons, and the second is the
  // one that decides it:
  //
  //   - Below that size the answer could not change anything. The low-heap
  //     ceiling IS one batch, so a buffer smaller than one batch is already
  //     under it however the measurement comes out.
  //   - heap_caps_get_largest_free_block() walks the allocator's free list.
  //     Doing that on every line of a device that is merely logging normally
  //     would be this module paying a new per-line cost to solve a problem
  //     that only exists when the buffer is deep - the same mistake, in
  //     miniature, as the temporary String that line(const char*) exists to
  //     avoid.
  //
  // So the ordinary path costs one comparison, and the measurement happens
  // only on the pushes where it can actually decide something.
  if (bufferedBytes + textCost > kMaxBatchBytes || count + 1 > kMaxBatchLines) {
    largestBlock = Http::largestContiguousBytes();
    if (largestBlock < kMinLargestBlockBytes) {
      heapConstrained = true;
      lineCeiling = kMaxBatchLines;
      byteCeiling = kMaxBatchBytes;
    }
  }

  // DROPPING THE OLDEST IS THE RIGHT ANSWER, and it is worth saying why
  // rather than leaving it as "what the loop already did".
  //
  // An unsent log line is worth strictly less than the connection that would
  // send it. Keeping the line costs the heap that the POST draining it needs,
  // so a buffer that holds on is not preserving the line - it is guaranteeing
  // the line is never delivered AND taking the device off the air with it.
  // Everything still in here is only valuable if a session can still be
  // opened, which makes the session the thing to protect.
  //
  // Oldest-first, not newest-first, for two reasons. It is already the
  // policy at the byte cap directly above, and a device that quietly changed
  // which end it discarded depending on WHY it was discarding would be
  // unreadable. And the newest lines are the ones describing the condition
  // being diagnosed right now, which is what a reader opened the stream for.
  while (count > 0 && (count >= lineCeiling || bufferedBytes + textCost > byteCeiling)) {
    bufferedBytes -= buffer[head].length() + 1;
    buffer[head] = String();
    head = (head + 1) % kMaxBufferedLines;
    --count;
    ++droppedSinceFlush;
    if (heapConstrained) {
      ++droppedForHeapSinceFlush;
      largestBlockAtLastHeapDrop = largestBlock;
    }
  }

  if (count >= lineCeiling) {
    // A single line bigger than the entire buffer cap - drop it too rather
    // than looping forever trying to make room in a buffer that can never
    // fit it.
    ++droppedSinceFlush;
    return;
  }

  const size_t tail = (head + count) % kMaxBufferedLines;
  buffer[tail] = text;
  bufferedBytes += textCost;
  ++count;
}

/// Sends up to kMaxBatchLines/kMaxBatchBytes worth of the oldest buffered
/// lines. Only what the server actually accepted (HTTP 200) is removed from
/// the buffer - a failed POST leaves it untouched so nothing is lost beyond
/// what the buffer-cap eviction above already dropped for capacity reasons,
/// and the same lines are simply retried on the next poll().
void sendOneBatch() {
  if (count == 0 && droppedSinceFlush == 0) {
    return;
  }

  JsonDocument doc;
  JsonArray lines = doc["lines"].to<JsonArray>();

  if (droppedSinceFlush > 0) {
    lines.add("[" + String(droppedSinceFlush) + " lines dropped]");
  }

  if (droppedForHeapSinceFlush > 0) {
    // SAID OUT LOUD, because a cap that applies itself silently is
    // indistinguishable from a quiet device, and this one changes what the
    // reader is looking at: the stream in front of them is shallower than
    // usual, and the reason for that is very probably the thing they opened
    // it to investigate. The standing rule for this firmware is that CAL and
    // App log their decisions AND the branches they skipped, because the
    // remote stream is the only diagnostic channel a deployed device has -
    // a buffer silently deciding to be smaller is exactly such a decision.
    //
    // COMPOSED INTO THE OUTGOING BATCH, NOT LOGGED, and that is the whole
    // trick. This must not go through Log::printf() or Log::line(), because
    // both end in pushToBuffer() - the function that produced the drop. A log
    // line about the log buffer being over its ceiling, appended to the log
    // buffer, would evict another line, count another drop, and give the next
    // flush something new to complain about. Writing it straight into the
    // JSON array is the one place the buffer can say this about itself
    // without feeding itself.
    //
    // Nothing is owed to a USB reader here. line() writes Serial
    // unconditionally and FIRST, before pushToBuffer() is reached at all, so
    // every one of these dropped lines was already printed in full over the
    // cable. The drop is a property of the remote stream alone, which is what
    // Log.h's "fallback of last resort" requires and why this change could
    // not regress it even in principle.
    lines.add("[" + String(droppedForHeapSinceFlush) +
              " of those dropped for heap: largest free block was " +
              String(static_cast<unsigned long>(largestBlockAtLastHeapDrop)) +
              " and a fresh TLS session needs " +
              String(static_cast<unsigned long>(Http::kTlsRecordBufferBytes)) +
              " twice, so this buffer held one batch instead of its full ceiling rather than pin "
              "the memory the POST draining it needs]");
  }

  size_t taken = 0;
  size_t bytesTaken = 0;
  while (taken < count && taken < kMaxBatchLines) {
    const String& candidate = buffer[(head + taken) % kMaxBufferedLines];
    const size_t cost = candidate.length() + 1;
    if (taken > 0 && bytesTaken + cost > kMaxBatchBytes) {
      break;
    }
    lines.add(candidate);
    bytesTaken += cost;
    ++taken;
  }

  if (!Http::ready()) {
    return;  // try again next poll(); nothing consumed from the buffer
  }

  const String url = String("https://") + Config::kServiceHost + kPath;
  if (!Http::beginRequest(url)) {
    return;
  }
  HTTPClient& http = Http::client();
  http.setTimeout(Config::kHttpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());
  http.addHeader("Content-Type", "application/json");

  String body;
  serializeJson(doc, body);
  const int status = http.POST(body);
  http.end();

  if (status != 200) {
    // Left in the buffer to retry on the next poll(); the eviction cap above
    // is still what keeps this from growing unbounded if the outage lasts.
    return;
  }

  for (size_t i = 0; i < taken; ++i) {
    bufferedBytes -= buffer[head].length() + 1;
    buffer[head] = String();
    head = (head + 1) % kMaxBufferedLines;
    --count;
  }
  droppedSinceFlush = 0;
  // Cleared together with the total, and only on the HTTP 200 path, so the
  // heap marker is retried alongside the lines it explains rather than being
  // lost on the one flush that failed to go out. The early return at the top
  // still tests droppedSinceFlush alone, which remains sufficient: every heap
  // drop increments both counters, so this one can never be non-zero while
  // that one is zero.
  droppedForHeapSinceFlush = 0;
  largestBlockAtLastHeapDrop = 0;
}

}  // namespace

void printf(const char* format, ...) {
  char scratch[256];
  va_list args;
  va_start(args, format);
  const int written = vsnprintf(scratch, sizeof(scratch), format, args);
  va_end(args);

  if (written < 0) {
    return;
  }
  if (static_cast<size_t>(written) >= sizeof(scratch)) {
    // Ran past this fixed scratch buffer. Rather than growing it (this runs
    // on every log call, including ones made from deep, stack-tight retry
    // loops), keep what fit and say so, so a truncated line reads as
    // truncated instead of silently cut off mid-word.
    static const char kMarker[] = "...(truncated)";
    constexpr size_t kMarkerLen = sizeof(kMarker) - 1;
    memcpy(scratch + sizeof(scratch) - 1 - kMarkerLen, kMarker, kMarkerLen);
    scratch[sizeof(scratch) - 1] = '\0';
  }

  // scratch is already a C string - handing it straight to the const char*
  // overload skips a String temporary that was built and destroyed on every
  // single log line whether or not anyone was listening.
  line(scratch);
}

void verbose(const char* format, ...) {
  // The whole point of this function: bail before even touching va_list or
  // the scratch buffer below when nobody is watching the remote stream, so a
  // fleet-wide call site costs nothing on every device except the one an
  // admin actually turned streaming on for. printf() below duplicates this
  // function's own formatting instead of the two sharing a helper - the
  // early return here has to happen before formatting, not after, which
  // makes "format, then decide" the one shape that cannot serve both.
  if (!streaming) {
    return;
  }

  char scratch[256];
  va_list args;
  va_start(args, format);
  const int written = vsnprintf(scratch, sizeof(scratch), format, args);
  va_end(args);

  if (written < 0) {
    return;
  }
  if (static_cast<size_t>(written) >= sizeof(scratch)) {
    // Same truncation marker as printf() above, for the same reason.
    static const char kMarker[] = "...(truncated)";
    constexpr size_t kMarkerLen = sizeof(kMarker) - 1;
    memcpy(scratch + sizeof(scratch) - 1 - kMarkerLen, kMarker, kMarkerLen);
    scratch[sizeof(scratch) - 1] = '\0';
  }

  // Same const char* overload as printf() above. This path only runs with
  // streaming on, so the buffer copy's String does get built - but inside
  // line(), once, rather than here as a temporary that is then copied again.
  line(scratch);
}

void line(const String& text) {
  Serial.println(text);

  if (streaming) {
    pushToBuffer(text);
  }
}

void line(const char* text) {
  if (text == nullptr) {
    return;
  }

  Serial.println(text);

  // The String is constructed HERE and nowhere else - inside the one branch
  // that genuinely needs one, because pushToBuffer stores a String. With
  // streaming off, which is every device almost all of the time, this
  // performs no heap allocation at all. See Log.h's remarks on why that
  // matters specifically at this call site's frequency.
  if (streaming) {
    pushToBuffer(String(text));
  }
}

void setStreamingEnabled(bool enabled) {
  if (enabled == streaming) {
    return;
  }

  if (!enabled) {
    sendOneBatch();
    clearBuffer();
  } else {
    lastFlushMs = millis();
  }

  streaming = enabled;
}

bool streamingEnabled() { return streaming; }

void poll() {
  if (!streaming) {
    return;
  }

  const uint32_t now = millis();
  const bool timerElapsed = (now - lastFlushMs) >= kBatchIntervalMs;
  const bool bufferPressured = count >= kMaxBatchLines || bufferedBytes >= kMaxBatchBytes;
  if (!timerElapsed && !bufferPressured) {
    return;
  }

  lastFlushMs = now;
  sendOneBatch();
}

void flushNow() {
  if (!streaming) {
    return;
  }
  sendOneBatch();
  lastFlushMs = millis();
}

}  // namespace Log
