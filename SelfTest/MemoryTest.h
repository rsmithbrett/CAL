#pragma once

#include <Arduino.h>

/// Category 4: how much contiguous memory this specific unit actually has
/// right now - the exact question hours of manual telemetry/debug-log
/// correlation were trying to answer tonight for the read-buffer
/// fragmentation bug (see App/Display.cpp's own remarks on
/// readFileToBuffer()/ensureFileBufferCapacity()).
namespace MemoryTest {

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
