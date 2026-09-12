#pragma once

#include <Arduino.h>

/// Attributes the loss of contiguous heap to the phase of the ordinary card
/// rotation that was running when it happened, and carries the answer on
/// telemetry rather than on the debug stream.
///
/// ---------------------------------------------------------------------------
/// WHAT THIS IS FOR, AND WHAT IS ALREADY RULED OUT
/// ---------------------------------------------------------------------------
///
/// The figure that matters on this fleet is
/// `heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)`. Not free total, and
/// emphatically not `ESP.getMaxAllocHeap()`, which returns a constant 32,756 on
/// this board and has produced several wrong diagnoses - one of which survived
/// an entire evening. A new TLS session needs 16,717 contiguous bytes, twice
/// over, in two flat allocations (Http::kTlsRecordBufferBytes), so the largest
/// SINGLE block is the binding constraint. "Roughly 32KB" is a misreading of
/// that requirement and cost another evening.
///
/// Measured on device 17 (v2026.09.11.0017-customlibs, debug stream on), twice,
/// to the minute: 65,524 bytes at boot, SOFTWARE_RESET+UNREACHABLE 34 minutes
/// later. The steps down are multiples of 2,048 with plateaus between them, and
/// free total falls more slowly and does not plateau in step - which is
/// fragmentation carving the largest block, not a steady leak. The waypoints
/// recur across boots (65,524 -> ... -> 42,996 -> 36,852 ...) but their ORDER
/// differs between runs while the total time to failure matches. That is a
/// small set of repeated allocation sizes being reached in a varying order:
/// deterministic enough to find, and exactly what this module is here to name.
///
/// **The asset path is not it.** HeapTrace instrumented that at eight
/// transitions and it was ruled out; RGB565 draws were then measured five times
/// with the largest free block identical before and after each one. HeapTrace
/// stays for what it covers - the RAM-asset path on a card-less device - and
/// this module covers what is left, which is everything else in the rotation
/// and is where the decay has to be.
///
/// **This must work with the debug stream OFF.** The stream is itself the
/// dominant consumer (200 String slots, a 16KB ceiling, a JsonDocument and a
/// body String per batch - see Log.cpp), device 19 has never streamed and has
/// never decayed, and all streams are now off fleet-wide deliberately, to
/// measure real-world reboots. An instrument that needs the stream to be read
/// would measure a device in a state nobody is trying to fix any more. So the
/// product of this module is a handful of small integers on the telemetry POST,
/// which is fixed-size, flat, and still gets through when the stream does not -
/// the same argument that put bootLargestFreeBlockBytes and sdMountCostBytes on
/// the wire last night. The log lines below it are a bonus for whoever does
/// turn a stream on, never the deliverable.
///
/// ---------------------------------------------------------------------------
/// HOW THE ATTRIBUTION WORKS, AND WHY IT IS SIGNED
/// ---------------------------------------------------------------------------
///
/// One running floor (the largest block as of the last observation) and one
/// current phase. Every observation reads the largest block, credits the whole
/// signed difference from the floor to the phase in force, and moves the floor.
/// Nothing is thresholded away, so by construction:
///
///   bootLargestFreeBlock - floor  ==  sum of the five phase buckets
///
/// That identity is the point, and it is why the buckets are SIGNED rather than
/// a tally of downward steps only. Three things follow from it that a
/// drops-only counter would not give:
///
///   - A phase that allocates 44KB and hands all of it back nets ~0 and is
///     visibly NOT the culprit. A phase that carves 2,048 and keeps it reads
///     +2,048. Those are the two cases the whole investigation is trying to
///     tell apart, and gross allocation volume cannot tell them apart at all.
///   - A negative bucket is a phase that gives contiguous heap BACK - the free
///     list coalescing - which is real and worth seeing rather than clamping to
///     zero and calling noise.
///   - The server can CHECK this instrument against the two figures telemetry
///     already sends. If the buckets do not account for
///     (bootLargestFreeBlockBytes - largestFreeBlock8BitBytes), then either a
///     phase is missing a Scope or the carving happens somewhere none of the
///     five phases cover. An instrument that can be caught being wrong from its
///     own output is worth more than one that cannot.
///
/// Observations happen only at phase BOUNDARIES, never mid-phase, and that is
/// deliberate. A boundary is a quiescent point: whatever a phase allocated and
/// freed inside itself is gone by the time its Scope destructs, so the floor
/// tracks the true ratchet instead of chasing transient dips it will never see
/// recovered. Sampling mid-phase would record a 44KB decode scratch as a 44KB
/// loss and then never credit the free, which is precisely the wrong answer.
///
/// ---------------------------------------------------------------------------
/// WHAT IT COSTS - because this is a memory investigation and an instrument
/// that moves the thing it measures is worse than no instrument
/// ---------------------------------------------------------------------------
///
///   - Heap: ZERO. No String, no JsonDocument, no malloc, on any path. Every
///     label this module stores is a `const char*` pointing at a string literal
///     in flash (phase names here, card ids from Cards::CardSpec::id, the four
///     fixed subject literals at the Scope call sites) - pointers are copied,
///     never the characters. This property is load-bearing, not incidental: an
///     instrument that allocated would carve the very heap it is measuring and
///     the measurement would include itself.
///   - Static RAM: 46 bytes of state (5 int32 buckets, a floor, a step count, a
///     worst-step triple, the current phase and subject), call it 48 with
///     alignment.
///   - Stack: 12 bytes per live Scope - two pointers and a byte - and the
///     deepest nesting this firmware reaches is two (a Draw inside a Fetch, or
///     a Service inside a CheckIn). 24 bytes, against the headroom StackWatch
///     reports.
///   - Telemetry wire: eight new flat fields, ~190 bytes of body text. Eight
///     more ArduinoJson slots at 8 bytes each inside the pool the document
///     already holds - ARDUINOJSON_POOL_CAPACITY is 128 slots on this 32-bit
///     target and the telemetry document uses roughly 20, so this allocates NO
///     new pool. Keys and the one string value are literals, which ArduinoJson
///     links rather than copies. The body String may cross one realloc
///     boundary; that is a transient inside the Service phase and this module
///     measures it, which is the correct self-consistent answer rather than an
///     unaccounted cost.
///   - CPU: two heap_caps_get_largest_free_block() calls per Scope plus one per
///     loop() iteration. That walk is O(free blocks) under a lock, tens of
///     microseconds, against a loop paced at 50ms. HeapTrace already does three
///     per line and eight lines per asset cycle.
///
/// Flash is the one thing this spends freely, and flash is not the scarce
/// resource on this board.
///
/// ---------------------------------------------------------------------------
/// WHAT THIS EXPECTS TO FIND, RANKED, SO THE PREDICTION IS ON RECORD FIRST
/// ---------------------------------------------------------------------------
///
/// Written down before the first device came back, deliberately. An
/// investigation that has already produced two confident wrong diagnoses is one
/// where "we suspected that all along" is worth nothing unless it was said
/// beforehand, and a hypothesis that cannot be embarrassed by the data is not a
/// hypothesis. Each of these predicts a DIFFERENT bucket, which is the only
/// reason five buckets are enough.
///
/// 1. **A long-lived String built on top of a JsonDocument that is then freed
///    underneath it.** Predicts Fetch.
///
///    Every provider does this, identically. Forecast.cpp is the clearest:
///    `doc` is alive from the deserializeJson() at :147 until the function
///    returns at :204, and `result.location`, `info.name`, `info.unit` and
///    `info.shortForecast` are all allocated in between, at :155 and :178-182.
///    So the sequence per fetch is: take one or more variant pools, allocate
///    fifteen-odd small Strings ABOVE them, free the pools. What is left is a
///    pool-sized hole with live Strings sitting on top of it, and the Strings
///    outlive the fetch because they are copied into a file-static SharedSlot.
///
///    This is the hypothesis the observed arithmetic fits best.
///    ARDUINOJSON_POOL_CAPACITY is 128 slots on this 32-bit target and a slot is
///    8 bytes, so ArduinoJson takes its variant pools in units of exactly 1,024
///    bytes - and the steps measured on device 17 are multiples of 2,048, which
///    is two of them. It also explains the part that looked strangest: the
///    waypoints recur but their ORDER differs between runs, while the total time
///    matches. Each provider has its own refresh timer, so which one lands first
///    varies per boot, while the number of fetches per unit time does not.
///
/// 2. **`drawChrome()`'s per-draw String churn.** Predicts Draw.
///
///    Every single card draw copies up to three Actions::Definitions - three
///    Strings each - into gButtons, then builds three more Strings for the
///    labels, then destroys all of them. A dozen small allocate/free pairs on a
///    path that runs once per dwell, leaking nothing and carving steadily. This
///    is second rather than first because the carve sizes are tens of bytes, not
///    thousands, and the observed steps are quantised at 2,048. If the mean step
///    (total decay / heapRatchetSteps) comes back at 40 instead of 2,048, this
///    moves to first and hypothesis 1 is dead.
///
/// 3. **The check-in response's surviving Strings.** Predicts CheckIn.
///
///    The largest parse this firmware does, and the one that leaves the most
///    behind: the card policy, the action definitions, the announcements, all
///    copied into module state and all replaced wholesale on the next check-in -
///    new allocations placed before the old ones are released. Ranked third only
///    because check-in happens every few minutes rather than every dwell, so it
///    has far fewer chances to ratchet than 1 or 2 do.
///
/// 4. **The debug stream itself.** Predicts Service, and is ALREADY partly
///    evidenced - device 19 has never streamed and has never decayed - which is
///    exactly why it cannot be tested right now: every stream on the fleet is
///    off. If Service comes back non-trivial on a device with streaming off, it
///    is telemetry's own JsonDocument and body String, which is a much smaller
///    and much more surprising finding.
///
/// 5. **None of the above.** Predicts Idle, and is the reason that bucket
///    exists. The WiFi and LWIP stacks allocate continuously and nothing in this
///    firmware has ever looked at them. A device whose decay sits in Idle has
///    eliminated the four hypotheses above in one figure.
///
/// The honest position on all five: they are readings of this codebase, not
/// measurements. Nothing here has been compiled - there is no host C++ compiler
/// on the machine this was written on and arduino-cli hangs there - and every
/// device was offline when it was written. The first boot after they return is
/// what settles it.
namespace HeapRatchet {

/// The five buckets, chosen to discriminate between the candidates the
/// rotation actually offers rather than to cover every function.
///
/// The candidates, and which bucket each one lands in:
///
///   - LovyanGFX decode scratch and sprites, the RGB565 band buffer, the
///     per-card draw Strings, and the Actions label churn (drawChrome() copies
///     up to three Definitions - three Strings each - and then builds three
///     more Strings for the labels, so a dozen small allocate/free pairs land
///     on EVERY card draw)                                          -> Draw
///   - The provider fetches: a TLS request, a JsonDocument whose variant pools
///     come in 1,024-byte blocks, an HTTP body String on the failure paths, and
///     the long-lived Result Strings each provider builds WHILE its document is
///     still alive                                                  -> Fetch
///   - Check-in: the ~4KB CheckIn::Result, the largest JSON parse this firmware
///     does, and the policy and announcement Strings it leaves behind
///                                                                  -> CheckIn
///   - The housekeeping POSTs - telemetry, the debug-stream flush, the fallback
///     update check. One bucket for all three because they are the same shape
///     (a small fixed POST) and would be fixed the same way; splitting them
///     costs three wire fields to separate three things nobody would treat
///     differently. The subject on the log line still names which one.
///                                                                  -> Service
///   - Everything else: the WiFi and LWIP stacks, touch sampling, the timers,
///     and - this is the important part - anything this instrumentation FORGOT.
///                                                                  -> Idle
///
/// A large Idle bucket is therefore not a boring result. It is this module
/// reporting that the carving happens outside all four named phases, which
/// rules out every candidate listed above in one figure and sends the next
/// person somewhere entirely different. That is the outcome worth designing
/// for, because it is the one an instrument that only covered its own
/// hypotheses could never produce.
enum class Phase : uint8_t {
  Idle = 0,
  Draw,
  Fetch,
  CheckIn,
  Service,
  kCount,
};

/// Seeds the floor from the figure BootDiag already captured in setup(), so
/// this module's arithmetic and the bootLargestFreeBlockBytes the server
/// already receives anchor to the same instant.
///
/// Taking it as an argument rather than reading BootDiag directly keeps the
/// dependency pointing one way - App.ino wires the two together, the same shape
/// HeapTrace::setGraphicsActive() uses so that Assets and Graphic need not know
/// about each other. Passing 0 (BootDiag's own "not measured") makes this take
/// its own reading instead, so the module still works if the call order in
/// setup() is ever rearranged.
void begin(uint32_t bootLargestFreeBlock);

/// Samples the largest block and credits the difference to the phase in force.
///
/// Called by Scope on both ends of every phase, and once per loop() iteration
/// so that a long stretch with no Scope at all - a device sitting on one card
/// with nothing due - still gets sampled and still lands in Idle. Without that
/// loop-top call the Idle bucket would only ever be credited at the moment some
/// other phase began, and a device that stopped rotating would look like a
/// device that stopped losing memory.
void observe();

/// Sets the phase for a lexical block and restores the previous one on the way
/// out, observing at both ends.
///
/// RAII rather than an enter()/leave() pair, and this is not stylistic. Every
/// site this wraps has multiple exits - drawCurrent() returns early when there
/// is nothing showable, fetchCard() returns after show(), performCheckIn()
/// returns on a failed check-in and never returns at all on the reprovision
/// path - and a leave() that some exit forgets does not fail loudly. It
/// silently bills one phase's cost to another for the rest of the boot, which
/// produces a confident wrong answer. That is the failure mode this
/// investigation has already been burned by twice from other instruments, and
/// it is not worth risking again to save a class.
///
/// Nesting restores rather than resets, so a Draw inside a Fetch (a refresh
/// that redraws the card currently on screen) bills the draw to Draw and the
/// rest of the fetch to Fetch. Separating those two is most of the point:
/// without the nesting, every refresh of the visible card would charge its
/// redraw to the fetch and the two leading candidates would be inseparable.
class Scope {
 public:
  /// `subject` must outlive the Scope and is expected to be a string literal or
  /// a Cards::CardSpec::id - both live for the process. Only the pointer is
  /// kept; nothing is copied and nothing is allocated. Never pass a
  /// String::c_str().
  Scope(Phase phase, const char* subject);
  ~Scope();

  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  Phase previousPhase_;
  const char* previousSubject_;
};

/// The signed net bytes of contiguous 8-bit heap credited to one phase since
/// begin(). Positive means the phase took contiguous heap and did not give it
/// back; negative means the free list coalesced more than the phase consumed.
int32_t netBytes(Phase phase);

/// How many observations moved the floor DOWNWARD. Sent beside the buckets so
/// the mean step size is arithmetic the server can do: the claim these steps
/// are multiples of 2,048 was made by reading a stream line by line, and this
/// is what lets it be checked on a fleet with every stream off.
uint16_t stepCount();

/// The largest single downward step since begin(), and the phase it happened
/// in. One big carve and fifty small ones decay a heap at the same rate and
/// want completely different fixes, and the buckets alone cannot tell them
/// apart.
uint32_t worstStepBytes();
const char* worstStepPhaseName();

/// The subject that was in force at the worst step - a card id, or one of the
/// fixed service literals. Not sent on the wire (a card id is only meaningful
/// once the phase has already been narrowed down, and phase is the field that
/// discriminates between the candidates) but printed on the log line, so a
/// stream turned on afterwards names the exact card.
const char* worstStepSubject();

}  // namespace HeapRatchet
