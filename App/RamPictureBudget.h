#pragma once

#include <stddef.h>

/// Whether a picture may be held in RAM, given what a TLS session needs from
/// the same pool.
///
/// A device with no SD card has nowhere to cache a picture, so the only way to
/// draw one at all is to hold the bytes in RAM until the next refresh
/// (Assets::fetchToRam). On a board whose heap is internal DRAM only, those
/// bytes come out of the pool `mbedtls_ssl_setup()` allocates its record
/// buffers from, and spending that pool takes the device off the network: it
/// cannot check in, cannot be sent a card policy, and cannot be given a
/// firmware update. Assets.h's releaseRamBuffer() records the measurements that
/// failure was read from.
///
/// **There are two rules here, not one, because the two picture paths have
/// measurably different shapes.** Which one a caller wants is a question about
/// how many pictures it holds and what it can prove before allocating.
///
/// `fitsBesideTls()` is a PREDICTION made before the allocation, and it models
/// the picture as coming out of the largest free block. That model suits a card
/// holding several pictures at once, each large enough to need a block of its
/// own - the house-hunt card, up to five at nine to twenty-five kilobytes each.
///
/// `leavesHandshakePossible()` is a MEASUREMENT taken after the allocation, and
/// it asks only whether a handshake is still possible. It exists because the
/// prediction is false on at least one real device. Device 12 holds 15,937
/// bytes of picture buffers while its largest internal block sits at 17,396 -
/// and that figure does not move when a picture is allocated. 129 heaptrace
/// readings on 2026-11-02, every one of them 17,396, through
/// `beforeAssetFetch`, `afterResponseHeaders` and `afterRamAlloc` alike, while
/// `freeInt` moved from 32,168 to 24,848 underneath it. The pictures are not
/// carved out of that block; they come from elsewhere in the pool and leave it
/// whole. The device had 25 check-ins and zero handshake failures in the same
/// window.
///
/// So a prediction of the `fitsBesideTls()` shape refuses every picture device
/// 12 draws today, and has drawn for weeks, on the strength of a model that
/// device disproves. Measuring after the fact refuses only what actually
/// breached the floor. A caller retrofitting a guard onto a path that already
/// ships pictures wants the measurement.
///
/// **This header names nothing from Arduino, ESP-IDF or the graphics library**,
/// which is what lets the host test walk the range a device can only sample.
/// Every figure it needs arrives as an argument, the build-dependent ones
/// included: the caller reads them from HeapPools, Http and its own sdkconfig
/// and passes them in.
namespace RamPictureBudget {

/// The default `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` in both generated
/// sdkconfigs in this repository. An allocation at or below it is placed in
/// internal DRAM even on a board with PSRAM registered, so it competes with a
/// handshake exactly the way an allocation on a PSRAM-less board does.
///
/// Only load-bearing for a picture small enough to fall under it. The smallest
/// device-shaped listing picture the server has produced is 9,923 bytes, so in
/// practice every real picture is placed in PSRAM where PSRAM exists - but a
/// rule that is right because of what the data happens to hold is a rule that
/// breaks when the data moves.
constexpr size_t kAlwaysInternalThresholdBytes = 4096;

/// What a handshake asks of the internal pool on THIS build.
///
/// **Two figures, not one, and they answer different questions.**
/// `largestSingleBytes` is the biggest single allocation
/// `mbedtls_ssl_setup()` makes, so a pool whose largest block is under it
/// cannot stand up a session at any total. `totalBytes` is everything a session
/// holds at once, which a pool can fail to supply while still holding one block
/// that size.
///
/// **And the totals differ between the two trees this repository builds**,
/// which is why this is a parameter rather than a constant. A stock core sizes
/// both record buffers from `MBEDTLS_SSL_MAX_CONTENT_LEN 16384`, so 16,717
/// twice, 33,434 - that is the Panellan build. The overlaid core sets
/// `MBEDTLS_ASYMMETRIC_CONTENT_LEN` with IN 16384 and OUT 4096, so 16,717 in
/// and 4,429 out, 21,146 - that is the ESP32-2432S028 build. Read from the two
/// generated sdkconfigs on 2026-11-02. A single hardcoded "twice over" is wrong
/// by 12,288 bytes on one of them, and wrong in the strict direction, which is
/// the direction that silently withdraws a working picture.
///
/// `transient` is `CONFIG_MBEDTLS_DYNAMIC_BUFFER`, set on the ESP32 build and
/// absent from the Panellan one. Where it is set the record buffers are
/// allocated around a handshake and released after it, so `totalBytes` is a
/// peak the pool has to meet briefly rather than a standing reservation. It is
/// carried here because it is the difference between "this pool is permanently
/// short" and "this pool is short right now", and a caller that only measures
/// cannot tell those apart on its own.
struct TlsDemand {
  size_t largestSingleBytes = 0;
  size_t totalBytes = 0;
  bool transient = false;
};

/// Whether a handshake could begin at all with this much contiguous internal
/// heap.
///
/// A PROOF OF IMPOSSIBILITY when it returns false, in the same sense
/// Http::canOpenNewSession() is: below one record buffer there is no block big
/// enough for even the first of them, and no amount of retrying changes that.
/// Returning true is weaker - it says the first buffer fits, not that the whole
/// session does. The asymmetry is deliberate and is what makes the false case
/// safe to act on.
///
/// A demand of zero is refused rather than treated as "nothing needed": it
/// means the caller failed to derive its build's figures, and answering yes
/// would turn that into a guard that passes everything.
inline bool handshakePossible(size_t largestTlsBlockBytes, const TlsDemand& demand) {
  return demand.largestSingleBytes != 0 && largestTlsBlockBytes >= demand.largestSingleBytes;
}

/// The PREDICTION. True when `pictureBytes` can be held without displacing a
/// handshake, modelling the picture as coming out of the largest free block.
///
/// `largestTlsBlockBytes` is the largest contiguous block in the pool mbedTLS
/// allocates from - HeapPools::largestTlsBlockBytes(), measured with
/// `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`, never the bare 8-bit figure. On a
/// board with PSRAM the bare figure runs in the megabytes while the internal
/// pool may not hold a record buffer, so passing it here would describe an
/// unreachable device as having room to spare.
///
/// `placedInExternalRam` says where the bytes will actually land, and the
/// caller has to make that true rather than hope for it:
/// Assets::ensureRamBufferCapacity() allocates with `MALLOC_CAP_SPIRAM` when
/// HeapPools::externalRamInHeap() is true, so the picture cannot fall back into
/// internal DRAM when PSRAM is full. Reading the configuration and trusting
/// malloc to follow it would make this return true for an allocation that
/// quietly came out of the pool it was supposed to spare.
///
/// A zero-byte picture is refused: there is nothing to draw, and treating it as
/// free would let a response with no Content-Length through a check meant to
/// bound a real allocation.
inline bool fitsBesideTls(size_t pictureBytes, size_t largestTlsBlockBytes,
                          const TlsDemand& demand, bool placedInExternalRam) {
  if (pictureBytes == 0) {
    return false;
  }

  // In PSRAM, and above the threshold that would have pulled it back into
  // internal DRAM, the picture and the handshake are not drawing on the same
  // pool at all.
  if (placedInExternalRam && pictureBytes > kAlwaysInternalThresholdBytes) {
    return true;
  }

  const size_t reserve = demand.totalBytes;
  if (reserve == 0) {
    return false;
  }

  // Written as a subtraction on the pool rather than an addition to the
  // picture, so a picture size near SIZE_MAX cannot wrap the comparison into
  // passing.
  if (largestTlsBlockBytes < reserve) {
    return false;
  }
  return pictureBytes <= largestTlsBlockBytes - reserve;
}

/// The MEASUREMENT. True when a handshake is still possible now that the
/// picture has been allocated.
///
/// `largestTlsBlockAfterBytes` is read AFTER the allocation, which is the whole
/// point: it is the state of the pool rather than a model of it. A caller that
/// gets false here releases the buffer it just took and draws without the
/// picture.
///
/// **Why this does not also check the total.** On a build with transient
/// buffers the total is a peak met and released around each handshake, and a
/// pool momentarily short of it recovers on its own, so refusing on the total
/// would withdraw pictures that work. On a build without them the total does
/// matter, but nothing here can measure whether the pool can supply two
/// separate blocks that size - only the largest one. So this checks the floor
/// it can actually prove and leaves the total to `fitsBesideTls()`, which is
/// the rule for the path that holds several pictures and can afford to be
/// strict.
///
/// A picture placed in external RAM cannot have moved the internal figure, so
/// the caller is told yes however that figure reads: a false reading there
/// would be some other allocation's doing, and releasing this picture would not
/// fix it.
inline bool leavesHandshakePossible(size_t largestTlsBlockAfterBytes, const TlsDemand& demand,
                                    bool placedInExternalRam) {
  if (placedInExternalRam) {
    return true;
  }
  return handshakePossible(largestTlsBlockAfterBytes, demand);
}

}  // namespace RamPictureBudget
