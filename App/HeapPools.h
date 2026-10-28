#pragma once

// sdkconfig.h before anything that reads a CONFIG_ macro below, for the reason
// Display.cpp gives at its own top: a CONFIG_ name that is undefined at the
// point it is tested compiles clean and takes the wrong branch.
#include "sdkconfig.h"

#include <Arduino.h>
// MALLOC_CAP_* for the two masks below. Included here rather than left to
// whatever Arduino.h happens to drag in, because kTlsCaps is a constexpr in this
// header and every translation unit that reads it needs the macros defined.
#include <esp_heap_caps.h>

/// Which heap a figure came out of, and which heap the TLS floor is graded
/// against.
///
/// **The two were the same number until this repository built for a second
/// board.** `MALLOC_CAP_8BIT` is a permission - "this memory allows 8/16/...-bit
/// accesses" - and it is orthogonal to `MALLOC_CAP_INTERNAL` and
/// `MALLOC_CAP_SPIRAM` (see esp_heap_caps.h, where all three are separate bits).
/// On a chip with no external RAM registered in the heap, a bare
/// `MALLOC_CAP_8BIT` query and an `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` query
/// walk the same regions and return the same figure. Every heap number this
/// fleet has recorded was taken with the bare capability, and Telemetry.cpp's
/// own measurement on device 17 records all four classes (8BIT, INTERNAL|8BIT,
/// DMA, DEFAULT) agreeing to the byte.
///
/// The Panellan ZX2D80CE02S-V10 has 2MB of QSPI PSRAM and is built with
/// `PSRAM=enabled`, so `psramAddToHeap()` registers that PSRAM as an 8-bit
/// capable heap region. A bare `MALLOC_CAP_8BIT` largest-free-block query on
/// that board then reports the largest hole in internal DRAM **or** PSRAM,
/// whichever is bigger, which is a figure in the megabytes for the whole life of
/// the device.
///
/// **mbedTLS cannot use that PSRAM.** Both generated sdkconfigs in this
/// repository set `CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y`, and the ESP-IDF mbedTLS
/// port wires `MBEDTLS_PLATFORM_STD_CALLOC` to `esp_mbedtls_mem_calloc` (see
/// port/include/mbedtls/esp_config.h), which under that option allocates with
/// `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`. So every allocation a TLS session
/// makes comes out of internal DRAM: both 16,717-byte record buffers, and every
/// small MPI the certificate chain's four ECDSA P-384 links are verified with.
/// `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` does not enter into it - that
/// threshold steers `malloc()`, and mbedTLS does not go through `malloc()`.
///
/// Which gives the rule this module exists to hold in one place: **a figure that
/// gates TLS is measured with kTlsCaps, never with the bare capability.** A bare
/// figure from a board with PSRAM says a device has megabytes of room while its
/// internal pool cannot hold a record buffer, and the device is unreachable
/// while reporting maximum health into the debug stream that is the only channel
/// left for noticing.
///
/// Nothing here changes a number on the ESP32-2432S028. `esp_psram_init()` finds
/// no chip on an ESP32-D0WD-V3, no PSRAM region is ever registered, and both
/// capability masks resolve to the same regions they always did.
namespace HeapPools {

/// The capability mask every TLS-relevant figure is measured with, and the one
/// `esp_mbedtls_mem_calloc()` itself allocates with under
/// `CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC`.
constexpr uint32_t kTlsCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

/// The bare capability, kept because the fields already on the wire
/// (`free8BitBytes`, `largestFreeBlock8BitBytes`) are defined as this and a
/// field's meaning does not get to change underneath a server that reads it.
constexpr uint32_t kAnyCaps = MALLOC_CAP_8BIT;

// The premise of kTlsCaps, asserted rather than assumed.
//
// If a future core tree switches to CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC or
// CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC, mbedTLS starts drawing from PSRAM and the
// internal mask stops being the pool a handshake is graded against - every
// figure below would then be measuring the wrong thing while still compiling and
// still looking plausible. The build stops here instead, and whoever is holding
// the new core tree gets to decide what the floor means on it.
#if !defined(CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC)
#error "HeapPools::kTlsCaps assumes CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC. This core tree sets a different MBEDTLS mem-alloc option, so mbedTLS no longer allocates from internal DRAM and the TLS heap floor has to be re-derived before this builds."
#endif

/// Whether external RAM is registered in the heap on this device, right now.
///
/// A runtime question, not a build one. A board can be built with PSRAM enabled
/// and still come up without it: `esp_psram_init()` fails, `psramAddToHeap()`
/// refuses, and no SPIRAM region is ever registered. Reading the heap is the
/// only answer that describes the device in front of you.
bool externalRamInHeap();

/// "internal" on a device with no external RAM in the heap, "internal+psram"
/// where there is. Printed by every line that reports a bare-capability figure,
/// so a reader of the debug stream can tell which pools a number spans without
/// knowing which board sent it.
const char* anyPoolLabel();

/// Internal 8-bit heap: the pool a TLS session allocates from.
size_t largestTlsBlockBytes();
size_t freeTlsBytes();

/// Bare 8-bit heap, spanning PSRAM where it is registered. The existing wire
/// fields and log figures.
size_t largestAnyBlockBytes();
size_t freeAnyBytes();

}  // namespace HeapPools
