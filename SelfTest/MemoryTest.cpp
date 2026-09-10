#include "MemoryTest.h"

#include <cstdlib>
#include <cstring>

// heap_caps_* rather than ESP.getFreeHeap()/ESP.getMaxAllocHeap(). This suite
// exists to answer "how much contiguous memory does this unit actually have",
// and the Arduino wrappers were measured on this board answering that question
// wrong by about 4x: 49,960 free / 32,756 largest reported at a moment when
// MALLOC_CAP_8BIT held 11,340 free and a 6,132-byte largest block. A memory
// test reporting the optimistic number is not a weaker test, it is a
// misleading one - it would have declared this unit healthy on the exact
// night its 5,686-byte decode could not allocate.
#include <esp_heap_caps.h>

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
    entry.freeHeapBefore = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    entry.maxAllocHeapBefore = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    void* block = malloc(sizes[i]);
    entry.allocationSucceeded = (block != nullptr);
    if (block != nullptr) {
      // Actually touch every byte, not just receive a non-null pointer -
      // malloc() succeeding and the memory being genuinely usable are not
      // always the same fact on a fragmented or marginal heap.
      memset(block, 0xA5, sizes[i]);
    }

    entry.freeHeapAfter = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    entry.maxAllocHeapAfter = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    // The log says free8/largest8 even though the struct and the JSON still say
    // freeHeap/maxAllocHeap: those two are a wire contract with the server's
    // SelfTestReportRequest and renaming them would break older firmware's
    // report, but the log line is read by a person and should name the
    // measurement it now actually carries. See MemoryTest.h.
    Log::printf(
        "[memtest] alloc %u bytes: %s (free8 %u -> %u, largest8 %u -> %u)", sizes[i],
        entry.allocationSucceeded ? "ok" : "FAILED", entry.freeHeapBefore, entry.freeHeapAfter,
        entry.maxAllocHeapBefore, entry.maxAllocHeapAfter);

    if (block != nullptr) {
      free(block);
    }
  }
  return results;
}

}  // namespace MemoryTest
