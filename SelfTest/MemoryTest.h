#pragma once

#include <Arduino.h>

/// Category 4: how much contiguous memory this specific unit actually has
/// right now - the exact question hours of manual telemetry/debug-log
/// correlation were trying to answer for the read-buffer fragmentation bug.
/// (App no longer buffers whole files at all; it streams. This suite still
/// asks the allocation question directly, which is why it kept its point when
/// that code went away - see SelfTest/Display.h's remarks on
/// drawPngFromSdTest().)
namespace MemoryTest {

/// **The two heap fields are named after the Arduino wrappers but no longer
/// come from them.** `freeHeap*` now carries
/// heap_caps_get_free_size(MALLOC_CAP_8BIT) and `maxAllocHeap*` carries
/// heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) - see MemoryTest.cpp for
/// the measurement showing the wrappers overstate by ~4x on this board. The
/// names stay because they are the JSON keys in the self-test report the
/// server already parses (SelfTestReportRequest), and the server must keep
/// accepting reports from firmware up to six months old; renaming the wire
/// field to match the improved source would break exactly that. Read them as
/// "8-bit free" and "8-bit largest block" regardless of what they are called.
struct AllocResult {
  uint32_t requestedBytes = 0;
  bool allocationSucceeded = false;
  uint32_t freeHeapBefore = 0;
  uint32_t freeHeapAfter = 0;
  uint32_t maxAllocHeapBefore = 0;
  uint32_t maxAllocHeapAfter = 0;
};

constexpr uint8_t kMaxResults = 6;

struct Results {
  AllocResult allocations[kMaxResults];
  uint8_t count = 0;
};

/// Attempts allocations at the exact sizes implicated in tonight's bug
/// (2KB, 34KB, 80KB) plus two stress sizes above that (100KB, 150KB) to find
/// where this particular unit's heap actually gives out - every attempt is
/// freed immediately after its before/after heap snapshot is taken, so this
/// measures each size's isolated impact rather than compounding allocations
/// against each other.
Results run();

}  // namespace MemoryTest
