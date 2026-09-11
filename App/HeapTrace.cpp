#include "HeapTrace.h"

#include <esp_heap_caps.h>

#include "Log.h"

namespace HeapTrace {
namespace {

/// Internal 8-bit heap specifically. See HeapTrace.h on why this is
/// MALLOC_CAP_INTERNAL rather than the bare MALLOC_CAP_8BIT every other figure
/// on this fleet has used.
constexpr uint32_t kCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

uint8_t gGraphicsActive = 0;

/// Short, stable, greppable. Deliberately not the enum's C++ name and
/// deliberately not prose: these are grouped and diffed by an operator reading
/// a stream, and a stage label that changed wording between builds would break
/// every comparison made across an upgrade.
const char* name(Stage stage) {
  switch (stage) {
    case Stage::BeforeAssetFetch:     return "beforeAssetFetch";
    case Stage::AfterResponseHeaders: return "afterResponseHeaders";
    case Stage::AfterRamAlloc:        return "afterRamAlloc";
    case Stage::AfterDownload:        return "afterDownload";
    case Stage::BeforeDecode:         return "beforeDecode";
    case Stage::AfterDecode:          return "afterDecode";
    case Stage::AfterDraw:            return "afterDraw";
    case Stage::AfterRamRelease:      return "afterRamRelease";
  }
  return "unknown";
}

}  // namespace

void setGraphicsActive(uint8_t count) { gGraphicsActive = count; }

void mark(Stage stage, const char* subject, size_t assetBytes) {
  // All three read back to back and before anything is formatted, so the
  // figures describe one instant rather than three points spread across the
  // cost of building a string. sprintf into Log's buffer allocates, and an
  // allocation between two of these reads would show up as heap movement this
  // module caused itself - which on a page hunting a few hundred bytes per
  // cycle is not a rounding error.
  const size_t largest = heap_caps_get_largest_free_block(kCaps);
  const size_t freeNow = heap_caps_get_free_size(kCaps);
  const size_t minFree = heap_caps_get_minimum_free_size(kCaps);

  // Log::printf, not Log::verbose. The whole point of these lines is to be
  // available from a device in the field that is misbehaving, and verbose is
  // gated on the debug stream being switched on for that device - which is
  // exactly the thing that stops working once a device drops below the TLS
  // floor. A trace that disappears in the failure it was added to measure is
  // the mistake this firmware has already made twice: [boot] lines emitted
  // before check-in authorises the stream, and the [sd] mount-cost line added
  // hours ago that has still never been seen because it prints in setup().
  //
  // One line per stage, eight per cycle. On device 7's ~10s dwell that is a
  // real volume, and it is affordable for as long as this question is open -
  // the stream is ring-buffered and the alternative is another night of
  // inference. Remove the wiring, not this function, once the ratchet is
  // located.
  //
  // key=value throughout so it parses. minFreeInt is the low-water mark since
  // boot, which is the field that separates "this cycle is tight" from "this
  // cycle was tight once and has been living on the edge ever since".
  Log::printf(
      "[heaptrace] stage=%s subject=%s largestInt=%u freeInt=%u minFreeInt=%u assetBytes=%u "
      "graphicsActive=%u",
      name(stage), subject != nullptr ? subject : "-", static_cast<unsigned>(largest),
      static_cast<unsigned>(freeNow), static_cast<unsigned>(minFree),
      static_cast<unsigned>(assetBytes), static_cast<unsigned>(gGraphicsActive));
}

}  // namespace HeapTrace
