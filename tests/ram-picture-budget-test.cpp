// App/RamPictureBudget.h: whether a picture may be held in RAM beside a TLS
// session, over the figures the fleet has actually reported rather than at one
// board's numbers.
//
// The rule exists because a device with no SD card can only draw a picture by
// holding it in RAM, and on a board whose heap is internal DRAM only those
// bytes come out of the pool mbedtls_ssl_setup() allocates its record buffers
// from. Getting that wrong does not show up as a missing picture; it shows up
// as a device that cannot check in, cannot be sent a policy and cannot be
// updated, which is the failure Assets.h's releaseRamBuffer() records.
//
// THERE ARE TWO RULES AND THEY DISAGREE ON PURPOSE. fitsBesideTls() predicts
// before allocating and models the picture as coming out of the largest free
// block. leavesHandshakePossible() measures after allocating and asks only
// whether a handshake is still possible. The second exists because the first is
// false on device 12, and the cases below pin that difference rather than
// assuming the two paths have the same shape.
//
// The figures were read from device_telemetry, imported_listing_images and the
// devices' own heaptrace lines on 2026-11-02, and are row counts on a date
// rather than standing facts:
//
//   Proto 2 (device 12)      Esp32_2432S028       no SD   internal largest 17,396
//   Workstation (device 32)  Esp32_2432S028       no SD   internal largest 32,756
//   device 43                PanellanZx2d80Ce02S  no SD   internal largest 81,908
//   device 44                PanellanZx2d80Ce02S  no SD   internal largest 81,908
//
//   device-shaped listing pictures: 9,923 to 24,512 bytes, mean 20,502 over the
//   117 approved on the account those devices belong to.
//
//   Proto 2's graphic and aircraft buffers, held simultaneously: 4,750, 11,187
//   and 4,368 bytes. Its largest internal block read 17,396 at every heaptrace
//   stage across 129 readings while those were held, and it logged 25 check-ins
//   and zero handshake failures in the same window.
//
// The two builds ask different amounts of the internal pool, which is why the
// demand is a parameter: the ESP32 tree sets MBEDTLS_ASYMMETRIC_CONTENT_LEN
// (IN 16384, OUT 4096) with MBEDTLS_DYNAMIC_BUFFER, so 16,717 + 4,429 = 21,146
// and transient; the Panellan tree is stock symmetric with no dynamic buffer,
// so 16,717 twice = 33,434 and standing. Read from both generated sdkconfigs.

#include <cassert>
#include <cstdio>

#include "RamPictureBudget.h"

using RamPictureBudget::fitsBesideTls;
using RamPictureBudget::handshakePossible;
using RamPictureBudget::kAlwaysInternalThresholdBytes;
using RamPictureBudget::leavesHandshakePossible;
using RamPictureBudget::TlsDemand;

// Http::kTlsRecordBufferBytes. Repeated rather than included because Http.h
// pulls in Arduino; Assets.cpp sees both and static_asserts they agree, so a
// move there stops that build rather than leaving this test grading the rule
// against a figure the firmware no longer uses.
constexpr size_t kTlsRecord = 16717;

// The two demands the two shipped trees produce, derived the way Assets.cpp
// derives them.
constexpr TlsDemand kAsymmetric{kTlsRecord, 21146, true};  // ESP32-2432S028
constexpr TlsDemand kSymmetric{kTlsRecord, 33434, false};  // Panellan

// The measured internal-pool figures, by the device that reported them.
constexpr size_t kProto2Internal = 17396;
constexpr size_t kWorkstationInternal = 32756;
constexpr size_t kPanellanInternal = 81908;

// The device-shaped picture sizes the server has actually produced.
constexpr size_t kSmallestPicture = 9923;
constexpr size_t kMeanPicture = 20502;
constexpr size_t kLargestPicture = 24512;

// Proto 2's three held buffers.
constexpr size_t kProto2Graphic2 = 4750;
constexpr size_t kProto2Graphic3 = 11187;
constexpr size_t kProto2Logo = 4368;

int main() {
  // ---------------------------------------------------------------------
  // The derived demands are two different figures. A single hardcoded "twice
  // over" is 12,288 bytes too strict on the ESP32 tree.
  // ---------------------------------------------------------------------
  assert(kAsymmetric.totalBytes == 21146);
  assert(kSymmetric.totalBytes == 33434);
  assert(kSymmetric.totalBytes - kAsymmetric.totalBytes == 12288);
  assert(kAsymmetric.largestSingleBytes == kSymmetric.largestSingleBytes);

  // ---------------------------------------------------------------------
  // handshakePossible(): the floor is one record buffer rather than the total.
  // This is what makes the false case a proof.
  // ---------------------------------------------------------------------
  assert(handshakePossible(kProto2Internal, kAsymmetric));  // 17,396 >= 16,717
  assert(handshakePossible(kTlsRecord, kSymmetric));        // exactly on it
  assert(!handshakePossible(kTlsRecord - 1, kSymmetric));   // one byte under
  assert(handshakePossible(kPanellanInternal, kSymmetric));

  // A demand nobody derived is refused rather than read as "nothing needed",
  // which would make every guard built on this pass everything.
  constexpr TlsDemand kUnderived{};
  assert(!handshakePossible(kPanellanInternal, kUnderived));
  assert(!handshakePossible(static_cast<size_t>(-1), kUnderived));

  // ---------------------------------------------------------------------
  // THE PREDICTION, on the two boards with no PSRAM. The answer is NOT a flat
  // "no picture ever", and the first draft of this test asserted that it was.
  //
  // Proto 2's 17,396 is under the asymmetric total on its own, so nothing of
  // any size is affordable there - a one-byte picture included. The refusal is
  // a property of the pool, not of the picture.
  // ---------------------------------------------------------------------
  assert(!fitsBesideTls(kSmallestPicture, kProto2Internal, kAsymmetric, false));
  assert(!fitsBesideTls(kMeanPicture, kProto2Internal, kAsymmetric, false));
  assert(!fitsBesideTls(kLargestPicture, kProto2Internal, kAsymmetric, false));
  assert(!fitsBesideTls(1, kProto2Internal, kAsymmetric, false));

  // Workstation's 32,756 DOES hold the asymmetric total, leaving 11,610. So the
  // smallest picture the server has produced FITS there and the mean and
  // largest do not. "Neither ESP32-2432S028 can hold a picture at any size" was
  // read off the stock 33,434 figure and is wrong for this board on this build;
  // the boundary is 11,610 and 9,923 is under it.
  assert(fitsBesideTls(kSmallestPicture, kWorkstationInternal, kAsymmetric, false));
  assert(!fitsBesideTls(kMeanPicture, kWorkstationInternal, kAsymmetric, false));
  assert(!fitsBesideTls(kLargestPicture, kWorkstationInternal, kAsymmetric, false));
  assert(fitsBesideTls(11610, kWorkstationInternal, kAsymmetric, false));
  assert(!fitsBesideTls(11611, kWorkstationInternal, kAsymmetric, false));

  // On the stock demand that same pool affords nothing, which is the 12,288
  // bytes the two trees differ by doing real work - and why the figure has to be
  // derived per build rather than picked once.
  assert(!fitsBesideTls(1, kWorkstationInternal, kSymmetric, false));
  assert(!fitsBesideTls(kSmallestPicture, kWorkstationInternal, kSymmetric, false));

  // ---------------------------------------------------------------------
  // THE PREDICTION, on the Panellan boards: the same pictures are allowed,
  // because the bytes are placed in PSRAM and the internal pool is untouched.
  // ---------------------------------------------------------------------
  assert(fitsBesideTls(kSmallestPicture, kPanellanInternal, kSymmetric, true));
  assert(fitsBesideTls(kMeanPicture, kPanellanInternal, kSymmetric, true));
  assert(fitsBesideTls(kLargestPicture, kPanellanInternal, kSymmetric, true));

  // A picture far larger than the whole internal pool is still allowed there,
  // which is the point of placing it elsewhere. 2MB is the registered PSRAM.
  assert(fitsBesideTls(2u * 1024u * 1024u, kPanellanInternal, kSymmetric, true));

  // PSRAM is not a blanket permission: placement is what matters. A Panellan
  // whose picture lands in internal DRAM anyway is graded against the internal
  // pool like any other board. 81,908 - 33,434 leaves 48,474.
  assert(fitsBesideTls(kMeanPicture, kPanellanInternal, kSymmetric, false));
  assert(fitsBesideTls(48474, kPanellanInternal, kSymmetric, false));
  assert(!fitsBesideTls(48475, kPanellanInternal, kSymmetric, false));

  // ---------------------------------------------------------------------
  // The ALWAYSINTERNAL threshold. A picture at or below it is placed in
  // internal DRAM whatever the board, so the PSRAM shortcut must not apply.
  // ---------------------------------------------------------------------
  constexpr size_t kRoomy = kSymmetric.totalBytes + kAlwaysInternalThresholdBytes;

  // Just above the threshold: PSRAM, allowed regardless of the pool.
  assert(fitsBesideTls(kAlwaysInternalThresholdBytes + 1, kProto2Internal, kSymmetric, true));

  // Exactly at it: internal, so the pool decides.
  assert(!fitsBesideTls(kAlwaysInternalThresholdBytes, kProto2Internal, kSymmetric, true));
  assert(fitsBesideTls(kAlwaysInternalThresholdBytes, kRoomy, kSymmetric, true));

  // ---------------------------------------------------------------------
  // The prediction's boundary, walked rather than sampled: a rule written with
  // >= where it meant > is wrong by one byte, and one byte is the difference
  // between a handshake and MBEDTLS_ERR_SSL_ALLOC_FAILED.
  // ---------------------------------------------------------------------
  constexpr size_t kReserve = kSymmetric.totalBytes;

  // A pool holding precisely the reserve affords a zero-byte picture, which is
  // refused on its own account, and nothing larger.
  assert(!fitsBesideTls(0, kReserve, kSymmetric, false));
  assert(!fitsBesideTls(1, kReserve, kSymmetric, false));

  // One byte more of pool affords precisely one byte of picture.
  assert(fitsBesideTls(1, kReserve + 1, kSymmetric, false));
  assert(!fitsBesideTls(2, kReserve + 1, kSymmetric, false));

  // A pool sized for the mean picture affords it exactly, not one byte more.
  assert(fitsBesideTls(kMeanPicture, kReserve + kMeanPicture, kSymmetric, false));
  assert(!fitsBesideTls(kMeanPicture + 1, kReserve + kMeanPicture, kSymmetric, false));

  // A pool smaller than the reserve affords nothing, and must not wrap into
  // affording everything. This is the subtraction the implementation is written
  // as rather than an addition on the picture.
  assert(!fitsBesideTls(1, kReserve - 1, kSymmetric, false));
  assert(!fitsBesideTls(0, 0, kSymmetric, false));
  assert(!fitsBesideTls(static_cast<size_t>(-1), kProto2Internal, kSymmetric, false));
  assert(!fitsBesideTls(static_cast<size_t>(-1), kReserve + 1, kSymmetric, false));

  // A zero-byte picture is refused wherever it would be placed, PSRAM included:
  // a response with no Content-Length must not read as free.
  assert(!fitsBesideTls(0, kPanellanInternal, kSymmetric, true));

  // An underived demand refuses every picture rather than passing them all.
  assert(!fitsBesideTls(kMeanPicture, kPanellanInternal, kUnderived, false));

  // ---------------------------------------------------------------------
  // Five of them, which is what the house-hunt card holds.
  //
  // The fetch path asks once per picture while the ones already held are still
  // allocated, so the pool shrinks as they accumulate and the rule has to keep
  // refusing on the way down rather than approving the first four and stranding
  // the fifth. On a PSRAM-less board it refuses all five.
  // ---------------------------------------------------------------------
  for (int i = 0; i < 5; ++i) {
    assert(!fitsBesideTls(kMeanPicture, kProto2Internal, kAsymmetric, false));
  }

  // On a pool with genuine room each approval shrinks what is left until the
  // reserve stops the next one. 130,000 has 96,566 above the 33,434 reserve,
  // which holds four mean pictures (82,008) and not five (102,510). 136,000 was
  // the first figure written here and it holds all five with 56 bytes to spare,
  // which is how this case started out asserting the wrong number.
  size_t pool = 130000;
  int held = 0;
  for (int i = 0; i < 5; ++i) {
    if (fitsBesideTls(kMeanPicture, pool, kSymmetric, false)) {
      pool -= kMeanPicture;
      ++held;
    }
  }
  assert(held == 4);
  assert(pool >= kReserve);

  // ---------------------------------------------------------------------
  // THE MEASUREMENT, and the case the whole second rule exists for.
  //
  // Proto 2 holds 4,750 + 11,187 + 4,368 = 20,305 bytes of graphic and logo
  // buffers while its largest internal block reads 17,396 at every stage. The
  // prediction refuses all three. The measurement allows all three, because
  // 17,396 is still above the 16,717 floor and the block demonstrably did not
  // move. Allowing them is the correct answer: that device has 25 check-ins and
  // no handshake failures while holding them.
  // ---------------------------------------------------------------------
  assert(!fitsBesideTls(kProto2Graphic2, kProto2Internal, kAsymmetric, false));
  assert(!fitsBesideTls(kProto2Graphic3, kProto2Internal, kAsymmetric, false));
  assert(!fitsBesideTls(kProto2Logo, kProto2Internal, kAsymmetric, false));

  assert(leavesHandshakePossible(kProto2Internal, kAsymmetric, false));
  assert(leavesHandshakePossible(kProto2Internal, kSymmetric, false));

  // The two rules disagree about this device, which is the point. If they ever
  // agree, one has been turned into the other and the retrofit has silently
  // become the thing it was written not to be.
  assert(fitsBesideTls(kProto2Graphic3, kProto2Internal, kAsymmetric, false)
         != leavesHandshakePossible(kProto2Internal, kAsymmetric, false));

  // The measurement still bites. An allocation that leaves the block under one
  // record buffer is refused, whichever tree it is on.
  assert(!leavesHandshakePossible(kTlsRecord - 1, kAsymmetric, false));
  assert(!leavesHandshakePossible(kTlsRecord - 1, kSymmetric, false));
  assert(!leavesHandshakePossible(0, kAsymmetric, false));
  assert(leavesHandshakePossible(kTlsRecord, kAsymmetric, false));

  // Measured in external RAM the internal figure is somebody else's doing:
  // releasing this picture would not raise it, so the caller is told yes even
  // on a reading that would otherwise refuse.
  assert(leavesHandshakePossible(0, kSymmetric, true));
  assert(leavesHandshakePossible(kTlsRecord - 1, kSymmetric, true));

  // An underived demand refuses, same as everywhere else - except in external
  // RAM, where the demand is not consulted at all.
  assert(!leavesHandshakePossible(kPanellanInternal, kUnderived, false));
  assert(leavesHandshakePossible(kPanellanInternal, kUnderived, true));

  printf("PASS: ram picture budget - both rules over 4 measured devices and 2 build shapes\n");
  return 0;
}
