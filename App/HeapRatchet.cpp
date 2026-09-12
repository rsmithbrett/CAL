#include "HeapRatchet.h"

#include <esp_heap_caps.h>

#include "Log.h"

namespace HeapRatchet {
namespace {

/// MALLOC_CAP_8BIT, bare, and deliberately NOT the MALLOC_CAP_INTERNAL|8BIT
/// pair HeapTrace uses.
///
/// On this no-PSRAM ESP32-D0WD-V3 the two are the same pool and every figure
/// either one produces is identical - HeapTrace's own header says so, and the
/// [heapdiag] lines that print both side by side have always agreed. The reason
/// to use the bare capability HERE is comparability: every number this
/// investigation has recorded, including the 65,524-at-boot and 16,717-floor
/// figures this module's whole design rests on, and including the
/// bootLargestFreeBlockBytes and largestFreeBlock8BitBytes fields telemetry
/// already sends, was taken with MALLOC_CAP_8BIT. The identity in HeapRatchet.h
/// is checked against those two fields, so this has to be measured the same way
/// they are or the check would be comparing two quantities that only happen to
/// coincide on this board.
constexpr uint32_t kCaps = MALLOC_CAP_8BIT;

/// A step worth a log line. Accounting has NO threshold - every byte is
/// credited to a bucket, which is what makes the identity exact - but a line
/// per boundary would be a few thousand lines across the 34 minutes device 17
/// takes to fail, and most of them would be the odd few bytes of String churn
/// settling.
///
/// 1,024 is half the smallest step ever observed on hardware (2,048, and every
/// step seen was a multiple of it). Half rather than equal so a real carve
/// cannot fall under the bar through some off-by-a-header-size difference on a
/// device nobody has measured yet, and so a 1,024-byte ArduinoJson variant pool
/// - the allocation granularity that makes the observed 2,048 quantum look like
/// exactly two of something - would be caught on its own rather than only in
/// pairs.
constexpr int32_t kNotableStepBytes = 1024;

const char* phaseName(Phase phase) {
  switch (phase) {
    case Phase::Idle:    return "idle";
    case Phase::Draw:    return "draw";
    case Phase::Fetch:   return "fetch";
    case Phase::CheckIn: return "checkin";
    case Phase::Service: return "service";
    case Phase::kCount:  break;
  }
  return "unknown";
}

/// Signed, so the buckets sum exactly to (boot floor - current floor). See
/// HeapRatchet.h on why a drops-only tally would not answer the question.
int32_t gNet[static_cast<uint8_t>(Phase::kCount)] = {0};

/// The largest block as of the last observation. Not the minimum ever seen and
/// not the current value - it is the reference every credit is measured from,
/// and it moves in both directions.
size_t gFloor = 0;

/// False until begin() has run. Guards against an observation taken before the
/// floor is seeded, which would credit the whole boot's heap to whichever phase
/// happened to be first and read as a spectacular finding.
bool gStarted = false;

Phase gPhase = Phase::Idle;

/// Never owned, never copied - see Scope's contract in the header. "-" rather
/// than nullptr so every log line has something to print and no call site has
/// to null-check.
const char* gSubject = "-";

uint16_t gSteps = 0;
uint32_t gWorstStepBytes = 0;
const char* gWorstStepPhase = "none";
const char* gWorstStepSubject = "-";

}  // namespace

void begin(uint32_t bootLargestFreeBlock) {
  // 0 is BootDiag's "not measured", so fall back to taking the reading here.
  // Slightly worse - setup() has run further by now, so the anchor is lower and
  // every bucket measured against it is correspondingly smaller - but a module
  // that silently did nothing because a call order changed would be worse
  // still, and this way the failure is a slightly conservative number rather
  // than a missing one.
  gFloor = (bootLargestFreeBlock > 0) ? static_cast<size_t>(bootLargestFreeBlock)
                                      : heap_caps_get_largest_free_block(kCaps);
  gStarted = true;

  Log::printf("[ratchet] attributing contiguous-heap loss from %u bytes; a new TLS session needs "
              "16717 twice over, so that is the distance this boot has to fall before the device "
              "goes unreachable",
              static_cast<unsigned>(gFloor));
}

void observe() {
  if (!gStarted) {
    return;
  }

  const size_t now = heap_caps_get_largest_free_block(kCaps);
  if (now == gFloor) {
    return;
  }

  // Computed as a signed difference of two size_t values via int64_t rather
  // than by subtracting them directly. `now - gFloor` on unsigned types wraps
  // to an enormous positive number whenever the heap RECOVERED, and a bucket
  // that silently accumulated 4 billion on every coalesce would look exactly
  // like the catastrophic finding this module exists to report. The whole
  // figure fits in int32 comfortably - the entire heap is ~320KB.
  const int32_t delta =
      static_cast<int32_t>(static_cast<int64_t>(gFloor) - static_cast<int64_t>(now));
  gFloor = now;

  gNet[static_cast<uint8_t>(gPhase)] += delta;

  if (delta < kNotableStepBytes) {
    // Includes every recovery (delta negative) and all the sub-quantum churn.
    // Accounted for above, just not narrated.
    return;
  }

  ++gSteps;
  if (static_cast<uint32_t>(delta) > gWorstStepBytes) {
    gWorstStepBytes = static_cast<uint32_t>(delta);
    gWorstStepPhase = phaseName(gPhase);
    gWorstStepSubject = gSubject;
  }

  // Log::printf, not Log::verbose, for the reason HeapTrace::mark() gives at
  // length: verbose is gated on the debug stream, and the stream is exactly
  // what stops working on a device that has fallen below the TLS floor - so a
  // trace that vanishes in the failure it was added to measure is no trace at
  // all. With the stream off this costs a Serial.println and no heap
  // whatsoever (see Log::line(const char*), which builds its String only inside
  // the streaming branch).
  //
  // This is a bonus channel, not the deliverable. All streams are off
  // fleet-wide right now by design, so the figures that have to survive are the
  // ones on the telemetry POST; this line is what pays off for whoever turns a
  // stream on afterwards and wants the exact card rather than just the phase.
  Log::printf("[ratchet] step -%d to %u during %s (%s), step %u this boot",
              static_cast<int>(delta), static_cast<unsigned>(now), phaseName(gPhase), gSubject,
              static_cast<unsigned>(gSteps));
}

Scope::Scope(Phase phase, const char* subject)
    : previousPhase_(gPhase), previousSubject_(gSubject) {
  // Observed BEFORE the switch, so the stretch that just ended is billed to the
  // phase that was actually running through it rather than to the one about to
  // start. Getting this order wrong would shift every cost one phase to the
  // right and produce a confident, consistent, completely wrong attribution.
  observe();
  gPhase = phase;
  gSubject = (subject != nullptr) ? subject : "-";
}

Scope::~Scope() {
  // Same argument, mirrored: observe while this phase is still in force, then
  // restore. The restore is what makes nesting work - see the header on why a
  // Draw inside a Fetch has to be separable.
  observe();
  gPhase = previousPhase_;
  gSubject = previousSubject_;
}

int32_t netBytes(Phase phase) {
  if (phase >= Phase::kCount) {
    return 0;
  }
  return gNet[static_cast<uint8_t>(phase)];
}

uint16_t stepCount() { return gSteps; }

uint32_t worstStepBytes() { return gWorstStepBytes; }

const char* worstStepPhaseName() { return gWorstStepPhase; }

const char* worstStepSubject() { return gWorstStepSubject; }

}  // namespace HeapRatchet
