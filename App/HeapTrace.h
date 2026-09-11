#pragma once

#include <Arduino.h>

/// One line per transition on the asset path, with the same five figures every
/// time, so the cost of each step is a subtraction rather than an inference.
///
/// **What this is for.** Device 7 sheds roughly 34KB of contiguous internal
/// heap per hour and crosses the ~16,717-byte floor a new TLS session needs,
/// after which it can render cards perfectly and reach nothing. It has no SD
/// card, so every asset load goes through Assets::fetchToRam() - a whole JPEG
/// pulled into one contiguous RAM buffer, decoded, drawn, and released. That
/// path is the only thing distinguishing it from the two devices whose problem
/// turned out to be the SD mount instead, and it is the remaining unexplained
/// mechanism on this fleet.
///
/// **The one question these lines exist to answer.** After
/// Assets::releaseRamBuffer() hands a buffer back, does largestInternalBlock
/// return to what it was before the fetch?
///
///   - If it does, there is no ratchet on this path and the decay is elsewhere.
///   - If it does NOT, the shortfall is the ratchet, measured per cycle, with
///     the exact transition it appears at named. That is hard evidence rather
///     than the correlation we have now.
///
/// Everything else these lines carry is there to stop a wrong answer to that
/// question looking like a right one - see the field notes below.
namespace HeapTrace {

/// The stages, in the order a single graphic card's cycle passes through them.
/// Named rather than free-text so a log can be grouped by stage without
/// matching on prose, and so a missing stage is visible as a gap in a sequence
/// rather than as nothing at all.
///
/// The pairs that matter are adjacent on purpose: AfterRamAlloc minus
/// BeforeAssetFetch is what the buffer cost, AfterDecode minus BeforeDecode is
/// what the decoder took and did not give back within the draw, and
/// AfterRamRelease minus BeforeAssetFetch is the ratchet for the whole cycle.
enum class Stage : uint8_t {
  BeforeAssetFetch,
  AfterResponseHeaders,
  AfterRamAlloc,
  AfterDownload,
  BeforeDecode,
  AfterDecode,
  AfterDraw,
  AfterRamRelease,
};

/// Emits one trace line for `stage`.
///
/// `subject` is the card or asset the cycle is about ("graphic5"), so two
/// instances interleaving in one rotation stay separable - on a device with two
/// graphic cards their cycles overlap, and a trace that did not say which card
/// it belonged to would read as one impossible sequence.
///
/// `assetBytes` is the size of the asset in play, or 0 where the stage does not
/// know it yet (BeforeAssetFetch, before any header has been read). Carried on
/// every line rather than logged once because the ratchet may well scale with
/// it, and a per-cycle figure with no size beside it cannot show that.
///
/// **On MALLOC_CAP_INTERNAL, not MALLOC_CAP_8BIT.** Every heap figure this
/// fleet has recorded so far used 8BIT alone. On a no-PSRAM ESP32-D0WD-V3 the
/// two are the same pool and the numbers are identical, so nothing already
/// measured is invalidated - but they are the same only by accident of this
/// board having no external RAM, and a figure that means "internal" should say
/// so. The existing [heapdiag] lines print both side by side and have always
/// agreed.
///
/// **And never ESP.getFreeHeap()/getMaxAllocHeap().** getMaxAllocHeap() reads a
/// constant 32,756 on this board while the true largest block has been measured
/// from 1,780 to 77,812. It has produced several wrong diagnoses, including one
/// that survived an entire evening, and it is the reason this module takes its
/// own measurements rather than accepting a caller's.
void mark(Stage stage, const char* subject, size_t assetBytes);

/// How many graphic instances are currently holding a RAM asset buffer.
///
/// Pushed in by Graphic.cpp rather than read out of it, so this module depends
/// on nothing: Assets.cpp calls mark() too, and having Assets reach into
/// Graphic for a count would invert the module graph for one integer.
///
/// It is on every line because the ratchet is expected to be per-buffer, and a
/// device with two graphic cards mid-rotation has two buffers live at once.
/// Without this, one cycle's figures would be silently contaminated by the
/// other card's buffer and the arithmetic would look non-deterministic.
void setGraphicsActive(uint8_t count);

}  // namespace HeapTrace
