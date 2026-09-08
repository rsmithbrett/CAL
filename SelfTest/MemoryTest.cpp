#include "MemoryTest.h"

#include <cstdlib>
#include <cstring>

#include "Log.h"

namespace MemoryTest {

Results run() {
  Results results;
  // 2KB/34KB/80KB mirror the exact read-buffer sizes from tonight's fixed
  // bug (see App/Display.cpp); 100KB/150KB push past them to find where
  // this specific unit's heap actually gives out, per the brief this sketch
  // was built against.
  const uint32_t sizes[] = {2 * 1024, 34 * 1024, 80 * 1024, 100 * 1024, 150 * 1024};

  for (uint8_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]) && i < kMaxResults; ++i) {
    AllocResult& entry = results.allocations[results.count++];
    entry.requestedBytes = sizes[i];
    entry.freeHeapBefore = ESP.getFreeHeap();
    entry.maxAllocHeapBefore = ESP.getMaxAllocHeap();

    void* block = malloc(sizes[i]);
    entry.allocationSucceeded = (block != nullptr);
    if (block != nullptr) {
      // Actually touch every byte, not just receive a non-null pointer -
      // malloc() succeeding and the memory being genuinely usable are not
      // always the same fact on a fragmented or marginal heap.
      memset(block, 0xA5, sizes[i]);
    }

    entry.freeHeapAfter = ESP.getFreeHeap();
    entry.maxAllocHeapAfter = ESP.getMaxAllocHeap();

    Log::printf(
        "[memtest] alloc %u bytes: %s (freeHeap %u -> %u, maxAllocHeap %u -> %u)", sizes[i],
        entry.allocationSucceeded ? "ok" : "FAILED", entry.freeHeapBefore, entry.freeHeapAfter,
        entry.maxAllocHeapBefore, entry.maxAllocHeapAfter);

    if (block != nullptr) {
      free(block);
    }
  }
  return results;
}

}  // namespace MemoryTest
