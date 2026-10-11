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
// per-POST batch cap below. Once either limit is hit, the oldest buffered
// message is dropped to make room for the newest, rather than growing forever
// or risking a failed allocation on a board this RAM-constrained.
//
// It used to be 200 lines / 16KB of Strings. It is now 32 slots of 256 bytes,
// declared with the rest of the storage further down - the depth came down
// because a buffer that deep was only ever worth having if the device stayed
// up long enough to send it, and this one is reserved whether streaming is on
// or not. See the cost table at kSlotCount.

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

// ---- The two budgets this module has to bound, and why one is not enough ----
//
// A log module on this board spends memory in two separate places, and capping
// only the first leaves the second free to do the damage:
//
//   1. THE STORED MESSAGES. Bounded below by one fixed byte ring, allocated
//      once as static storage and reused forever.
//   2. THE UPLOAD THAT DRAINS THEM. Bounded below by one fixed body buffer, so
//      composing a POST never allocates, and by a cap on attempts, so a server
//      that is not answering cannot keep re-running the allocation path.
//
// Budget 2 is the one that was invisible. The previous implementation built a
// JsonDocument holding the batch and then serialized it into a growing String -
// two transient copies of the same 4KB, peaking at exactly the moment the TLS
// session wanted its own record buffer. Bounding the queue alone would not have
// touched that overlap.
//
// WHAT WAS HERE BEFORE, so the change reads as a correction rather than taste:
// `String buffer[kMaxBufferedLines]` - 200 String objects whose text storage
// allocated and reallocated as messages came and went. Not 200 live allocations
// at rest, which is a thing worth being accurate about; the cost was the churn,
// a stream of differently-sized blocks taken and returned on every rotation.
// That bounds total bytes and says nothing about the SHAPE of the heap it
// leaves behind, which is what decides whether a contiguous TLS record buffer
// can still be found. Device 17 on 2026-10-09 sat at 24,356 bytes free 8-bit
// with a largest block of 14,836 - memory enough, in pieces too small.
//
// The ring below is 8KB rather than the old 16KB ceiling, deliberately: a
// 16,384-byte reservation is the same order as the 16,717-byte contiguous
// allocation it must never be the reason for failing, and the depth was only
// ever worth having if the device stayed up to send it.
// WHAT THIS COSTS, STATED PLAINLY, because static storage is not free memory -
// it is heap given up permanently in exchange for never having to find it:
//
//     slots     32 x 256 = 8,192 bytes
//     lengths   32 x   2 =    64 bytes
//     body                = 3,072 bytes
//     ------------------------------------
//     total               = 11,328 bytes
//
// That is ~11KB this device no longer has for anything else, on every boot,
// whether or not anybody ever turns streaming on. It buys a heap that cannot be
// fragmented by logging and an upload path that cannot fail to allocate. On a
// board whose defect is a contiguous 16,717-byte block that intermittently does
// not exist, a fixed cost that removes a variable one is the right trade - but
// it IS a trade, and shrinking the batch below is what keeps the bill at 11KB
// instead of 17KB.
constexpr size_t kSlotCount = 32;

/// 255 bytes of text plus the terminator. A message longer than that is stored
/// clipped and counted as a truncation, which is a different event from a drop:
/// a truncated line still tells the reader what happened, a dropped one does not.
constexpr size_t kSlotBytes = 256;
constexpr size_t kMaxTextBytes = kSlotBytes - 1;

/// The upload buffer, sized for the JSON as it is actually emitted rather than
/// for a worst-case escaping multiplier.
///
/// Escaping is accounted for by measuring each line's escaped length as the body
/// is built and stopping when the next one will not fit, so this is a hard
/// ceiling the composer respects rather than an estimate it hopes to stay under.
/// That removes the x2 headroom a worst-case sizing would have had to reserve,
/// which is 5KB of permanent RAM saved for a few lines of arithmetic.
constexpr size_t kBodyBytes = 3072;

/// How many times one batch may be offered before it is given up on.
///
/// Bounded work, not only bounded memory. A queue cap already stops the backlog
/// growing while a server is unreachable, but it does nothing about rebuilding
/// and re-POSTing the same batch every second forever - which re-runs the whole
/// allocation path indefinitely. After this many refusals the batch is dropped
/// and counted, exactly as a capacity eviction is.
constexpr uint8_t kMaxDeliveryAttempts = 3;

/// How long to wait after giving up on a batch before trying the next one.
constexpr uint32_t kBackoffMsAfterFailure = 15000;

bool streaming = false;

// ---- Budget 1: the stored messages ----
//
// Static, not heap. The strongest reading of "allocate once and reuse" is to
// never take it from the allocator at all: static storage is reserved at link
// time, cannot fail, needs no init hook to run before the first log line, and
// can never be the block that fragments the heap it was taken from.
//
// Fixed slots rather than a packed byte ring. A byte ring packs better, and the
// packing is not worth what it costs to read: every drain path grows a
// does-this-record-wrap case, and that is the half of a ring buffer that gets
// written wrong. This is the only diagnostic channel a deployed device has, so
// the indexing stays something that can be checked by eye - `head` plus an
// offset, modulo the slot count, and nothing else.
//
// The cost of the simplicity is the tail of each slot that a short line leaves
// unused. That is wasted address space, not wasted heap: the slots are reserved
// either way, so a half-empty one costs nothing that a packed one would have
// given back.
char gSlots[kSlotCount][kSlotBytes];
uint16_t gLens[kSlotCount];
size_t head = 0;
size_t count = 0;
size_t bufferedBytes = 0;

// ---- Budget 2: the upload ----
//
// Also static, and sized for the worst case rather than grown to fit. Composing
// a POST allocates nothing, so the batch that drains the queue cannot be what
// denies the TLS session underneath it the contiguous block it needs.
char gBody[kBodyBytes];

/// Refusals for the batch currently at the head of the ring.
uint8_t gDeliveryAttempts = 0;

/// Set when a batch is abandoned, so the next attempt waits rather than
/// following straight on.
uint32_t gBackoffUntilMs = 0;

/// Lines given up on after kMaxDeliveryAttempts, counted separately from
/// capacity evictions because they say a different thing: not "this device
/// logged faster than it could send" but "the server did not take these".
uint32_t droppedUndeliverable = 0;

/// Messages stored clipped at kMaxTextBytes. Counted apart from every drop
/// counter on purpose: a truncation and a drop are different losses, and a
/// reader who sees the stream thinning needs to know which one is happening.
/// Folded into one total they would be indistinguishable, and the remedies are
/// opposite - a truncation says a call site is too verbose for one line, a drop
/// says the device cannot keep up at all.
uint32_t truncatedSinceFlush = 0;

/// Set while a check-in is in flight. See Log::setUploadsSuppressed - this is
/// the whole of budget 2's second half, which is that two TLS sessions wanting
/// a record buffer at the same moment is a memory event even when each one
/// would have been affordable alone.
bool gUploadsSuppressed = false;
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

/// The slot holding the nth-oldest message. The whole of this module's indexing.
size_t slotAt(size_t offsetFromOldest) { return (head + offsetFromOldest) % kSlotCount; }

/// Removes the oldest message. The caller owns the drop counters, because the
/// two reasons a message leaves early - no capacity, or nobody would take it -
/// are counted separately and read differently.
void evictOldest() {
  if (count == 0) {
    return;
  }
  bufferedBytes -= gLens[head] + 1;
  gLens[head] = 0;
  gSlots[head][0] = '\0';
  head = (head + 1) % kSlotCount;
  --count;
}

void clearBuffer() {
  head = 0;
  count = 0;
  bufferedBytes = 0;
  droppedSinceFlush = 0;
  droppedForHeapSinceFlush = 0;
  largestBlockAtLastHeapDrop = 0;
  droppedUndeliverable = 0;
  truncatedSinceFlush = 0;
  gDeliveryAttempts = 0;
  gBackoffUntilMs = 0;
}

void pushToBuffer(const char* text) {
  if (text == nullptr) {
    return;
  }

  size_t length = strlen(text);
  if (length > kMaxTextBytes) {
    // 255 bytes of text plus the terminator is what a slot holds. printf() and
    // verbose() already format into a scratch buffer of the same size, so this
    // only fires for a caller that built a longer string by other means.
    // Clipping beats refusing, and it is counted apart from the drops so the
    // two losses stay distinguishable.
    length = kMaxTextBytes;
    ++truncatedSinceFlush;
  }

  const size_t textCost = length + 1;

  // The ceilings in force for THIS push. Normally the line/byte caps; one
  // batch's worth when the heap says the full ceiling is not affordable (see
  // kMinLargestBlockBytes).
  size_t lineCeiling = kSlotCount;
  size_t byteCeiling = kSlotCount * kSlotBytes;
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
    evictOldest();
    ++droppedSinceFlush;
    if (heapConstrained) {
      ++droppedForHeapSinceFlush;
      largestBlockAtLastHeapDrop = largestBlock;
    }
    // Evicting the head retires whatever the in-flight batch was counting from,
    // so the attempt tally restarts against whatever is now oldest rather than
    // condemning it for refusals it was never offered in.
    gDeliveryAttempts = 0;
  }

  if (count >= lineCeiling) {
    // A single line bigger than the entire buffer cap - drop it too rather
    // than looping forever trying to make room in a buffer that can never
    // fit it.
    ++droppedSinceFlush;
    return;
  }

  const size_t slot = slotAt(count);
  memcpy(gSlots[slot], text, length);
  gSlots[slot][length] = '\0';
  gLens[slot] = static_cast<uint16_t>(length);
  bufferedBytes += textCost;
  ++count;
}

/// How much of gBody is composed so far.
size_t gBodyLen = 0;

/// The closing "]}" every composed body needs. Reserved by every capacity check
/// below rather than hoped for at the end, so a body can never be filled to a
/// point where it cannot be finished.
constexpr size_t kClosingBytes = 2;

bool bodyAppendRaw(const char* text, size_t length) {
  if (gBodyLen + length + kClosingBytes > kBodyBytes) {
    return false;
  }
  memcpy(gBody + gBodyLen, text, length);
  gBodyLen += length;
  return true;
}

/// Appends one JSON string element, escaped, with its separating comma when it
/// is not the first.
///
/// MEASURED IN FULL BEFORE A BYTE IS WRITTEN. A half-written element is worse
/// than a dropped one: it is a body that parses as nothing, so the batch that
/// would have told somebody what was happening becomes a 400 instead. The
/// measuring pass is why capping by escaped bytes is affordable at all - it is
/// what lets gBody be 3KB rather than the 8.5KB a worst-case x2 reservation
/// would have cost in permanent RAM.
bool bodyAppendLine(const char* text, size_t length, bool first) {
  size_t needed = first ? 2 : 3;  // the two quotes, plus a comma when not first
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c == '"' || c == '\\') {
      needed += 2;
    } else if (c < 0x20) {
      needed += 6;  // \u00XX
    } else {
      needed += 1;
    }
  }

  if (gBodyLen + needed + kClosingBytes > kBodyBytes) {
    return false;
  }

  if (!first) {
    gBody[gBodyLen++] = ',';
  }
  gBody[gBodyLen++] = '"';
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c == '"' || c == '\\') {
      gBody[gBodyLen++] = '\\';
      gBody[gBodyLen++] = static_cast<char>(c);
    } else if (c < 0x20) {
      static const char kHex[] = "0123456789abcdef";
      gBody[gBodyLen++] = '\\';
      gBody[gBodyLen++] = 'u';
      gBody[gBodyLen++] = '0';
      gBody[gBodyLen++] = '0';
      gBody[gBodyLen++] = kHex[(c >> 4) & 0x0F];
      gBody[gBodyLen++] = kHex[c & 0x0F];
    } else {
      gBody[gBodyLen++] = static_cast<char>(c);
    }
  }
  gBody[gBodyLen++] = '"';
  return true;
}

/// Appends a marker the buffer writes about itself, composed into a stack
/// scratch rather than built with String concatenation - the old markers each
/// allocated several temporaries at precisely the moment the device was short
/// of memory, which is the moment they exist to describe.
bool bodyAppendMarker(bool first, const char* format, ...) {
  char scratch[192];
  va_list args;
  va_start(args, format);
  const int written = vsnprintf(scratch, sizeof(scratch), format, args);
  va_end(args);
  if (written <= 0) {
    return false;
  }
  size_t length = static_cast<size_t>(written);
  if (length > sizeof(scratch) - 1) {
    length = sizeof(scratch) - 1;
  }
  return bodyAppendLine(scratch, length, first);
}

/// Sends the oldest messages that fit one bounded batch.
///
/// Only what the server accepted (HTTP 200) leaves the buffer. A refusal keeps
/// the batch exactly as it is and offers it again, up to kMaxDeliveryAttempts -
/// and then gives it up and backs off, because retrying forever bounds memory
/// without bounding WORK, and re-running the compose-and-POST path every second
/// against a server that is not answering is the allocation churn this rewrite
/// exists to remove.
void sendOneBatch(bool ignoreSuppression = false) {
  if (count == 0 && droppedSinceFlush == 0 && truncatedSinceFlush == 0 &&
      droppedUndeliverable == 0) {
    return;
  }

  // Budget 2's second half. A log POST and a check-in POST each want a TLS
  // record buffer, and two of them live at once is a memory event even where
  // either alone would have been affordable. The log is the one that yields:
  // a check-in carries the card policy, the firmware manifest and the device's
  // only means of being managed, and a log batch waits a second with no loss.
  //
  // flushNow() passes ignoreSuppression, and that exception is the point rather
  // than an escape hatch: the pre-restart flush has no later poll() to fall
  // back on, so deferring it is not deferring anything - it is discarding the
  // lines that say why the device is about to restart.
  if (gUploadsSuppressed && !ignoreSuppression) {
    return;
  }

  const uint32_t now = millis();
  if (gBackoffUntilMs != 0 && static_cast<int32_t>(now - gBackoffUntilMs) < 0) {
    return;
  }

  gBodyLen = 0;
  if (!bodyAppendRaw("{\"lines\":[", 10)) {
    return;
  }
  bool first = true;

  if (droppedSinceFlush > 0) {
    if (bodyAppendMarker(first, "[%lu lines dropped]",
                         static_cast<unsigned long>(droppedSinceFlush))) {
      first = false;
    }
  }

  if (truncatedSinceFlush > 0) {
    // A DIFFERENT LOSS FROM A DROP, and said separately for that reason. A
    // truncated line still carries its prefix and most of its content, so the
    // reader learns what happened and loses only the tail; a dropped line is
    // gone entirely. Folded into one total they would be indistinguishable, and
    // they want opposite remedies - a truncation says one call site writes
    // lines too long for a slot, a drop says the device cannot keep up at all.
    if (bodyAppendMarker(first, "[%lu lines truncated at %u bytes]",
                         static_cast<unsigned long>(truncatedSinceFlush),
                         static_cast<unsigned>(kMaxTextBytes))) {
      first = false;
    }
  }

  if (droppedUndeliverable > 0) {
    if (bodyAppendMarker(first,
                         "[%lu lines given up on after %u refusals - the server was answering "
                         "something other than 200 and holding them would have kept rebuilding "
                         "this batch forever]",
                         static_cast<unsigned long>(droppedUndeliverable),
                         static_cast<unsigned>(kMaxDeliveryAttempts))) {
      first = false;
    }
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
    if (bodyAppendMarker(first,
                         "[%lu of those dropped for heap: largest free block was %lu and a fresh "
                         "TLS session needs %lu, so this buffer held one batch instead of its "
                         "full ceiling rather than pin the memory the POST draining it needs]",
                         static_cast<unsigned long>(droppedForHeapSinceFlush),
                         static_cast<unsigned long>(largestBlockAtLastHeapDrop),
                         static_cast<unsigned long>(Http::kTlsRecordBufferBytes))) {
      first = false;
    }
  }

  // Capped by what actually fits the body once escaped, not by an estimate of
  // it. A message that will not fit stays queued and leads the next batch,
  // which is why this breaks rather than skipping: the buffer is ordered, and
  // stepping over one line to fit a later one would deliver the stream out of
  // sequence.
  size_t taken = 0;
  while (taken < count && taken < kMaxBatchLines) {
    const size_t slot = slotAt(taken);
    if (!bodyAppendLine(gSlots[slot], gLens[slot], first)) {
      break;
    }
    first = false;
    ++taken;
  }

  // GUARANTEED PROGRESS. The arithmetic says this cannot happen - the longest
  // permitted message is 255 bytes, which escapes to at most 1,530, and the
  // body is 3,072 - but a batch that can never fit even one line would retry
  // forever and take the stream down with it, so the one case that would spin
  // is made to terminate rather than left to a proof.
  if (taken == 0 && count > 0 && gBodyLen == 10) {
    evictOldest();
    ++droppedUndeliverable;
    return;
  }

  if (!bodyAppendRaw("]}", kClosingBytes)) {
    return;
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

  // The composed bytes go straight out. No serializeJson into a String, and no
  // JsonDocument behind it - those were two transient copies of the batch,
  // peaking exactly while the session underneath wanted its record buffer.
  const int status = http.POST(reinterpret_cast<uint8_t*>(gBody), gBodyLen);
  http.end();

  if (status != 200) {
    ++gDeliveryAttempts;
    if (gDeliveryAttempts < kMaxDeliveryAttempts) {
      // Held exactly as composed and offered again. The buffer is untouched,
      // so the retry carries the same lines in the same order.
      return;
    }

    // Given up on. Bounded memory was never the whole requirement - bounded
    // work is the other half, and a batch retried without limit re-runs the
    // compose-and-POST path every second for as long as the outage lasts.
    for (size_t i = 0; i < taken; ++i) {
      evictOldest();
    }
    droppedUndeliverable += taken;
    gDeliveryAttempts = 0;
    gBackoffUntilMs = now + kBackoffMsAfterFailure;
    if (gBackoffUntilMs == 0) {
      gBackoffUntilMs = 1;  // 0 is the "no backoff" sentinel
    }
    return;
  }

  for (size_t i = 0; i < taken; ++i) {
    evictOldest();
  }
  gDeliveryAttempts = 0;
  gBackoffUntilMs = 0;
  truncatedSinceFlush = 0;
  droppedUndeliverable = 0;
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
    pushToBuffer(text.c_str());
  }
}

void line(const char* text) {
  if (text == nullptr) {
    return;
  }

  Serial.println(text);

  // No String anywhere on this path now. pushToBuffer copies into a slot it
  // already owns, so the temporary this branch used to build - once per line,
  // on the device's busiest code path - is gone rather than merely narrowed.
  if (streaming) {
    pushToBuffer(text);
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

void setUploadsSuppressed(bool suppressed) { gUploadsSuppressed = suppressed; }

bool uploadsSuppressed() { return gUploadsSuppressed; }

void poll() {
  if (!streaming) {
    return;
  }

  // Checked before the timer, so a suppressed window does not burn the flush
  // slot it was going to use: lastFlushMs is left alone and the batch goes out
  // on the first poll after the check-in finishes rather than a second later.
  if (gUploadsSuppressed) {
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
  sendOneBatch(/*ignoreSuppression=*/true);
  lastFlushMs = millis();
}

}  // namespace Log
