#pragma once

#include <Arduino.h>
#include <stdint.h>

/// What a display tells the server about pictures it downloaded, verified and
/// then could not draw.
///
/// **Apart from Assets.cpp so it can be run on a host.** Assets.cpp needs SD,
/// mbedTLS, LovyanGFX and four other modules, so the accounting inside it was
/// never executed by anything except a device - and it was wrong on the fleet for
/// as long as it shipped (see `record()`). This header names `String` and nothing
/// else, which is the whole of what `ci/test-decode-failure-report.sh` has to
/// stub. Same move `CardColumns.h` made, for the same reason.
///
/// One instance lives at file scope in Assets.cpp. Assets.h's public accessors
/// read it, CheckIn.cpp puts it on the request, and `clear()` runs once a check-in
/// carrying it has actually succeeded - so a failure is reported once per
/// occurrence rather than on every check-in afterwards.
namespace DecodeFailure {

/// Why a draw was given up on. Three causes, because they are the three a display
/// can actually tell apart, and they send a person to three different places.
///
/// `Display.cpp` already decided between them before this existed: it had to, to
/// choose whether to delete the cached file. What it did not do was say which, so
/// the server received an id and had to infer the rest from the asset catalog -
/// which cannot distinguish a picture that is wrong from a display that was short
/// of heap at that one moment.
enum class Reason : uint8_t {
  /// No draw path classified this one. Reported as absent rather than guessed at:
  /// an unrecognised image format reaches here, and calling that a decoder
  /// refusal would be inventing a finding.
  Unclassified = 0,

  /// The file wanted LovyanGFX's PNG decoder and this heap could not have held
  /// its 45,056-byte scratch in one block, so nothing ever reached a decoder.
  /// Nothing is wrong with the bytes and nothing about the picture will fix it.
  DecoderAbsent,

  /// The rgb565 band path asked `malloc` for a buffer of even one scanline and
  /// got null back. A measurement rather than a reading, unlike the two either
  /// side of it.
  AllocationRefused,

  /// A decoder ran, had the bytes, and would not read them. This is the only one
  /// of the three that is a statement about the picture.
  BytesRejected,
};

/// The name this reason travels under, or `nullptr` for `Unclassified`.
///
/// `nullptr` is what CheckIn.cpp omits the member on, which is how absent stays
/// distinguishable from a value. There is deliberately no name for "we don't
/// know": a reason code the server does not recognise means the display is newer
/// than the server, and that reading has to stay available.
inline const char* wireName(Reason reason) {
  switch (reason) {
    case Reason::DecoderAbsent:
      return "decoder-absent";
    case Reason::AllocationRefused:
      return "allocation-refused";
    case Reason::BytesRejected:
      return "bytes-rejected";
    case Reason::Unclassified:
      break;
  }
  return nullptr;
}

/// How many distinct pictures one report names. Four, as it always has been -
/// `Log`'s own line length and ArduinoJson's document both sized around it.
constexpr uint8_t kMaxReportedIds = 4;

/// One reporting cycle's worth of pictures that would not draw.
class Report {
 public:
  /// Takes one given-up draw.
  ///
  /// **This is where the shipped defect was.** The dedup used to ask whether the
  /// id appeared anywhere in the comma-joined id string, and ids stopped being
  /// appended once the count reached `kMaxReportedIds`. So the fifth and every
  /// later distinct id of a cycle was never in that string, therefore never
  /// matched by the dedup, and incremented the counter on every failed draw of
  /// it. The increment was a bare `uint8_t++` with no ceiling, so a display
  /// redrawing an unreportable picture every dwell rolled 255 back to 0 and the
  /// report then read as nothing having failed at all.
  ///
  /// Two things changed. The count rises only when an id is actually taken into
  /// the report, so it is the number of distinct pictures named and equal to the
  /// number of ids. And the dedup compares whole ids rather than searching for a
  /// substring, so it answers the question it was always meant to ask.
  ///
  /// A bounded report cannot dedup an id it has no room to remember, so a fifth
  /// picture is dropped rather than counted. That is the reading of the count
  /// that stays true at the cap, and `giveUps()` is what keeps the rest of the
  /// story.
  void record(const String& id, Reason reason, uint32_t bytes, uint32_t largestBlock) {
    // Saturates rather than wrapping, matching gAllocationRefusals in Assets.cpp.
    // This tally is the unbounded one - a card can refail every dwell - so the
    // ceiling is doing real work here rather than standing by.
    if (giveUps_ < UINT8_MAX) {
      ++giveUps_;
    }

    if (holds(id)) {
      return;
    }
    if (count_ >= kMaxReportedIds) {
      return;
    }

    ids_[count_] = id;
    ++count_;

    // The FIRST failure's three values, not the latest. By the time four
    // pictures have failed the heap is somewhere else entirely, and the first
    // failure is the only one that is not a consequence of the ones before it.
    // It is also the one the first id names, so the four read as one record.
    if (count_ == 1) {
      reason_ = reason;
      bytes_ = bytes;
      largestBlock_ = largestBlock;
    }
  }

  /// Back to nothing, including the three values. Called once a check-in carrying
  /// the report has succeeded.
  void clear() {
    for (uint8_t i = 0; i < kMaxReportedIds; ++i) {
      ids_[i] = "";
    }
    count_ = 0;
    giveUps_ = 0;
    reason_ = Reason::Unclassified;
    bytes_ = 0;
    largestBlock_ = 0;
  }

  /// Distinct pictures named, at most `kMaxReportedIds`.
  uint8_t count() const { return count_; }

  /// Draws given up on this cycle, which is not the same number and can be far
  /// larger. Logged rather than sent: the server reads a count of pictures, and
  /// adding a second count to the wire would need somewhere to put it.
  uint8_t giveUps() const { return giveUps_; }

  /// The ids, comma-joined, in the order they failed.
  String ids() const {
    String joined;
    for (uint8_t i = 0; i < count_; ++i) {
      if (i > 0) {
        joined += ",";
      }
      joined += ids_[i];
    }
    return joined;
  }

  bool hasAny() const { return count_ > 0; }

  /// The first failure's reason, byte size and largest contiguous 8-bit block.
  Reason reason() const { return reason_; }
  uint32_t bytes() const { return bytes_; }
  uint32_t largestBlock() const { return largestBlock_; }

  /// Whether each figure is worth putting on the wire.
  ///
  /// **Zero means the device could not measure it, and that is sent as absence
  /// rather than as a zero.** A display with no SD card has no file to size, and
  /// a 0 would read as a measurement of an empty file; a largest-block figure of
  /// 0 would read as a heap with nothing left in it. Neither is what happened, so
  /// neither is claimed.
  bool reportsBytes() const { return bytes_ > 0; }
  bool reportsLargestBlock() const { return largestBlock_ > 0; }

 private:
  /// Exact comparison against every id already taken, rather than a substring
  /// search over the joined list. Ids are uniform-length guids today, so the two
  /// agree on the fleet - but a substring test answers a different question than
  /// the one being asked, and it only stayed correct by accident of the id format.
  bool holds(const String& id) const {
    for (uint8_t i = 0; i < count_; ++i) {
      if (ids_[i] == id) {
        return true;
      }
    }
    return false;
  }

  /// Four Strings rather than one joined one. Costs four small objects against
  /// the one it replaces and buys an exact dedup; the characters held are the same
  /// characters either way, which on this heap is the figure that matters.
  String ids_[kMaxReportedIds];
  uint8_t count_ = 0;
  uint8_t giveUps_ = 0;
  Reason reason_ = Reason::Unclassified;
  uint32_t bytes_ = 0;
  uint32_t largestBlock_ = 0;
};

}  // namespace DecodeFailure
