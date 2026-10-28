#include "HeapPools.h"

#include <esp_heap_caps.h>

namespace HeapPools {

bool externalRamInHeap() {
  // Total size rather than free size, so a PSRAM region that happens to be
  // fully allocated still reports as present. The question is which pools the
  // bare capability spans, and that does not change with how full they are.
  return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
}

const char* anyPoolLabel() { return externalRamInHeap() ? "internal+psram" : "internal"; }

size_t largestTlsBlockBytes() { return heap_caps_get_largest_free_block(kTlsCaps); }

size_t freeTlsBytes() { return heap_caps_get_free_size(kTlsCaps); }

size_t largestAnyBlockBytes() { return heap_caps_get_largest_free_block(kAnyCaps); }

size_t freeAnyBytes() { return heap_caps_get_free_size(kAnyCaps); }

}  // namespace HeapPools
