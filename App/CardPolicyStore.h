#pragma once

// The card policy, in the form it is kept in across a restart.
//
// WHAT THIS IS FOR. Card policy lives in RAM and nowhere else, so any restart
// drops it: CardManager::draw() takes its guard clause and the panel reads
// "Nothing to show yet - waiting for the first update from the server" until the
// server answers again. App.ino measured the fleet restarting every 13 to 27
// minutes on 2026-09-11 under ordinary conditions, so a server outage longer
// than about a quarter of an hour empties most of the displays - and does it
// through power, the heap watchdog and a nudge at the wall, not only through the
// unreachable watchdog that v2026.11.07.0001 already stood down.
// OUTAGE_CONTENT_LOSS_REVIEW.md section 1 is the review that found it.
//
// With a policy in flash and the SD asset cache that already survives
// (Assets.cpp), a restarted display draws its rotation with no server at all.
//
// THE POLICY, NOT THE CONTENT. Every card's content - the forecast, the
// aircraft, the tide, the photographs - is still RAM only and still lost on a
// restart. Those cards come back with their own "nothing yet" states and fill in
// on the first check-in that works. What this file saves is the rotation: which
// cards exist, in what order, with what dwell, and the assetId/text/qrData/
// location that arrive on the policy entry itself. That is enough for a graphic,
// an announcement and a QR card to draw their real content from flash and the SD
// cache alone.
//
// WHERE IT LIVES AND WHY. NVS, through Preferences, the way App/Identity.cpp,
// App/Impressions.cpp and App/BootDiag.cpp already persist. Not SD: Assets.cpp
// records that some active devices have no SD card and fetch straight to RAM, so
// SD alone would leave exactly those units with nothing. Not SPIFFS: that
// partition belongs to CAL and the App mounts no filesystem on internal flash.
// The nvs partition is 0x5000 - five 4KB sectors - which is what the budget
// below is sized against.
//
// Only <stdint.h> and <stddef.h>, for the reason App/UnreachableHoldOff.h and
// App/ReportedCounter.h give: a header a host test can include as it ships needs
// no stubs. The NVS half, the Cards::Policy conversion and the logging all live
// in CardPolicyStore.cpp, which does name Arduino.
//
// NOT A WIRE FORMAT. These bytes are written and read by one binary on one
// device. They never cross a network, never reach the server, and are never read
// by another build - a format version this build does not recognise is refused
// outright rather than tolerated. So the standing six-month backward-
// compatibility mandate, which governs the check-in request and response, does
// not reach this file: there is no second party to stay compatible with. A
// firmware downgrade reads a newer blob, refuses it, and waits for the server,
// which is what the device does today.

#include <stddef.h>
#include <stdint.h>

/// Forward-declared rather than included, so this header still names nothing but
/// <stdint.h> and <stddef.h> and a host test can compile it as it ships. The
/// four functions at the bottom of this file are defined in
/// CardPolicyStore.cpp, which does include Cards.h and Arduino.
namespace Cards {
struct Policy;
}

namespace CardPolicyStore {

/// The stored format. Bumped whenever the byte layout below changes, which makes
/// every blob already in flash unreadable by this build - deliberately. A
/// reader that guessed at an unfamiliar layout would restore a rotation nobody
/// configured.
constexpr uint8_t kFormatVersion = 1;

/// The first two bytes of a blob. Guards against reading some other key's bytes
/// as a policy, which a version byte alone cannot: an arbitrary byte has a 1 in
/// 256 chance of matching one version and a 1 in 16 million chance of matching
/// this pair as well.
constexpr uint8_t kMagic0 = 'C';
constexpr uint8_t kMagic1 = 'P';

/// The most bytes a stored policy may take.
///
/// ONE NVS SECTOR'S WORTH, and that is the whole derivation. The nvs partition
/// is 0x5000 = 20,480 bytes, five 4KB sectors, and it already holds the device
/// secret, three remembered networks and roughly thirty small scalars. NVS
/// rewrites a blob by writing the replacement before erasing what it replaced,
/// so a blob of this size needs two of those five sectors free at the moment of
/// the write. A larger budget starts competing with the device secret for room,
/// and a partition that cannot be resized without a cable is the wrong place to
/// find that out.
///
/// What it buys, asserted in tests/card-policy-store-test.cpp rather than
/// estimated here: a full 35-card policy carrying no content strings is 1,566
/// bytes, leaving 2,530 for content. The realistic worst case an operator can
/// build - five announcements at the full 280 characters, five QR payloads at
/// 100, five asset ids at 48 and five locations - fits inside it. The
/// theoretical worst case, every content field filled on all 35 cards, does not,
/// and is refused rather than truncated.
constexpr uint16_t kMaxBlobBytes = 4096;

/// One entry per registered card, which is what Cards::kMaxPolicyCards caps a
/// policy at. Repeated rather than included because Cards.h names Arduino.
/// CardPolicyStore.cpp static_asserts that the two agree, so a change to the
/// registry is a compile error here rather than a policy quietly stored short.
constexpr uint8_t kMaxEntries = 35;

/// The string bounds, each matching the Cards.h constant of the same name. Same
/// static_assert in CardPolicyStore.cpp for each.
constexpr uint8_t kMaxIdLength = 32;
constexpr uint8_t kMaxAssetIdLength = 48;
constexpr uint16_t kMaxTextLength = 280;
constexpr uint16_t kMaxQrDataLength = 100;
constexpr uint8_t kMaxLocationLength = 16;

/// The blob's layout, in bytes, little-endian throughout. Written and read a
/// byte at a time rather than by casting a struct over the buffer: a blob read
/// out of NVS has no alignment guarantee, and a struct's padding is the
/// compiler's business rather than the format's.
///
///   header, 14 bytes
///     0      'C'
///     1      'P'
///     2      format version
///     3      entry count
///     4..5   total bytes, this header included
///     6..9   defaultDwellSeconds, int32
///     10..13 manualNavHoldSeconds, int32
///
///   each entry, 30 fixed bytes then five NUL-terminated strings
///     0      kind code - see KindCode
///     1      flags - bit 0 is allowBanner
///     2..5   order, int32
///     6..9   dwellSeconds, int32
///     10..13 notableDwellSeconds, int32
///     14..17 maxItems, int32
///     18..21 interleaveEvery, int32
///     22..25 effectiveFromUtc, uint32 epoch seconds
///     26..29 effectiveToUtc, uint32 epoch seconds
///     then   id, assetId, text, qrData, location
///
/// THE NUMBERS ARE STORED RAW, exactly as Cards::PolicyEntry held them, and not
/// as CardManager::applyPolicy() resolves them. A zero maxItems stays zero here
/// and resolves to Cards::kDefaultMaxItems on the way out, the same as it did
/// when the policy first arrived; a value past the 1-50 the contract allows is
/// stored as it came and clamped by applyPolicy() with its own log line. So the
/// restore path hands applyPolicy() the same figures the check-in did and gets
/// the same descriptors, with no second copy of the resolution rules to keep in
/// step. int32 rather than the int16/uint16 the descriptor ends up holding for
/// the same reason.
///
/// Strings are NUL-terminated rather than length-prefixed so a reader can hand
/// out pointers into the blob with no copy. They cost the same byte either way.
constexpr uint16_t kHeaderBytes = 14;
constexpr uint16_t kEntryFixedBytes = 30;
constexpr uint8_t kEntryStringCount = 5;

/// A policy entry's `kind`, which arrives as a string and is stored as a code.
///
/// Unstated covers an absent field and any value this build does not recognise,
/// and it restores as an empty string - which applyPolicy() reads as "leave the
/// card's built-in kind alone", exactly what the unrecognised string did on the
/// way in.
enum class KindCode : uint8_t {
  Unstated = 0,
  List = 1,
  Interstitial = 2,
};

/// One policy entry, as the codec takes and returns it.
///
/// The string members are borrowed, never owned. On the way out of a Reader they
/// point into the blob, so the blob has to outlive the Entry - which is why
/// CardPolicyStore.cpp copies each one into its Cards::PolicyEntry before
/// freeing the buffer.
struct Entry {
  const char* id = "";
  KindCode kind = KindCode::Unstated;
  int32_t order = 0;
  int32_t dwellSeconds = 0;
  int32_t notableDwellSeconds = 0;
  int32_t maxItems = 0;
  int32_t interleaveEvery = 0;
  bool allowBanner = false;
  uint32_t effectiveFromUtc = 0;
  uint32_t effectiveToUtc = 0;
  const char* assetId = "";
  const char* text = "";
  const char* qrData = "";
  const char* location = "";
};

/// Whether the blob in flash may be applied.
enum class Restore : uint8_t {
  /// Apply it. The rotation draws before the first check-in lands.
  Use,
  /// Nothing in flash - a device on its first boot, or one whose blob was
  /// refused. It waits for the server, which is what every device does today.
  NothingStored,
  /// A policy has already arrived from the server this boot, so the stored one
  /// is the older statement. **This is the rule that makes persisting safe.**
  ServerAlreadyAnswered,
  /// Already done once this boot. A second restore would re-apply a policy the
  /// rotation has moved on from.
  AlreadyRestored,
};

/// \param haveStored A blob was read out of flash and accepted by a Reader.
/// \param serverPolicyApplied CardManager has applied a policy that came from a
///        check-in response.
/// \param alreadyRestored A stored policy was applied earlier this boot.
constexpr Restore decideRestore(bool haveStored, bool serverPolicyApplied,
                                bool alreadyRestored) {
  // First, and ahead of everything: a stored policy must never replace a newer
  // one. The server's answer is always the newer of the two.
  if (serverPolicyApplied) {
    return Restore::ServerAlreadyAnswered;
  }
  if (alreadyRestored) {
    return Restore::AlreadyRestored;
  }
  if (!haveStored) {
    return Restore::NothingStored;
  }
  return Restore::Use;
}

/// Whether an arriving policy is worth a flash write.
enum class Decision : uint8_t {
  /// Write it, and record its hash.
  Write,
  /// The same bytes are already there. **The ordinary case**, and the reason
  /// this persists at all without wearing the part out.
  Unchanged,
  /// No policy on this check-in, so leave flash alone. "The server sent no
  /// policy" means keep what is in force, and what is in flash is part of that.
  NothingToStore,
  /// Past the budget, or nothing encoded. The stored blob is CLEARED rather
  /// than left in place: a kept one would resurrect the policy before this one
  /// on the next restart, which is worse than waiting for the server.
  TooLarge,
};

/// \param policyPresent The check-in response actually carried a cardPolicy.
/// \param encodedBytes What a Writer said the policy needs. Zero means the
///        encoder produced nothing.
/// \param haveStoredHash A hash of the last stored blob was found in flash.
/// \param encodedHash hash() over the bytes just encoded.
/// \param storedHash The hash found in flash. Meaningless unless
///        haveStoredHash.
constexpr Decision decideStore(bool policyPresent, uint16_t encodedBytes, bool haveStoredHash,
                               uint32_t encodedHash, uint32_t storedHash) {
  if (!policyPresent) {
    return Decision::NothingToStore;
  }
  if (encodedBytes == 0 || encodedBytes > kMaxBlobBytes) {
    return Decision::TooLarge;
  }
  // The flash-wear gate. ShouldSendCardPolicy on the server holds a device on
  // what it has and resends at most hourly, so an unchanged policy still arrives
  // around 24 times a day - about 8,800 writes a year against a part rated near
  // 100,000. Comparing first is what turns that into a write per edit.
  if (haveStoredHash && storedHash == encodedHash) {
    return Decision::Unchanged;
  }
  return Decision::Write;
}

/// FNV-1a, 32-bit, over the encoded blob. Not a security property - this
/// answers "are these the same bytes I stored last time", nothing more, and it
/// is read back from flash four bytes at a time so the comparison costs no
/// second buffer.
inline uint32_t hash(const uint8_t* bytes, uint16_t length) {
  uint32_t value = 2166136261u;
  if (bytes == nullptr) {
    return value;
  }
  for (uint16_t i = 0; i < length; ++i) {
    value ^= bytes[i];
    value *= 16777619u;
  }
  return value;
}

namespace detail {

/// strlen, bounded, with no <string.h>. Returns limit when no terminator is
/// found within it.
inline size_t boundedLength(const char* text, size_t limit) {
  if (text == nullptr) {
    return 0;
  }
  size_t length = 0;
  while (length < limit && text[length] != '\0') {
    ++length;
  }
  return length;
}

inline uint16_t read16(const uint8_t* at) {
  return static_cast<uint16_t>(static_cast<uint16_t>(at[0])
                               | static_cast<uint16_t>(static_cast<uint16_t>(at[1]) << 8));
}

inline uint32_t read32(const uint8_t* at) {
  return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8)
         | (static_cast<uint32_t>(at[2]) << 16) | (static_cast<uint32_t>(at[3]) << 24);
}

}  // namespace detail

/// Encodes a policy into a caller-supplied buffer.
///
/// **A null buffer is a measuring pass.** Construct with (nullptr, 0), add every
/// entry, and needed() is the exact byte count - which is how CardPolicyStore.cpp
/// sizes its one transient allocation instead of reserving kMaxBlobBytes it
/// almost never uses. The cursor advances whether or not there is room for it, so
/// needed() is right even when the buffer was too small.
class Writer {
 public:
  Writer(uint8_t* buffer, uint16_t capacity)
      : buffer_(buffer), capacity_(buffer == nullptr ? 0 : capacity) {}

  void begin(int32_t defaultDwellSeconds, int32_t manualNavHoldSeconds) {
    cursor_ = 0;
    entries_ = 0;
    overflowed_ = false;
    overLong_ = false;
    put8(kMagic0);
    put8(kMagic1);
    put8(kFormatVersion);
    put8(0);  // entry count, written by finish()
    put16(0);  // total bytes, written by finish()
    put32(static_cast<uint32_t>(defaultDwellSeconds));
    put32(static_cast<uint32_t>(manualNavHoldSeconds));
  }

  /// A string past its bound is refused rather than shortened, and the whole
  /// blob with it. Cards.h's standing rule: a truncated asset id is a perfectly
  /// well-formed id for some OTHER asset, so drawing the wrong picture is worse
  /// than drawing none - and the same reasoning covers a notice chopped off
  /// mid-sentence and a QR code that encodes something else.
  void add(const Entry& entry) {
    if (!fits(entry.id, kMaxIdLength) || !fits(entry.assetId, kMaxAssetIdLength)
        || !fits(entry.text, kMaxTextLength) || !fits(entry.qrData, kMaxQrDataLength)
        || !fits(entry.location, kMaxLocationLength)) {
      overLong_ = true;
      return;
    }

    put8(static_cast<uint8_t>(entry.kind));
    put8(entry.allowBanner ? 0x01 : 0x00);
    put32(static_cast<uint32_t>(entry.order));
    put32(static_cast<uint32_t>(entry.dwellSeconds));
    put32(static_cast<uint32_t>(entry.notableDwellSeconds));
    put32(static_cast<uint32_t>(entry.maxItems));
    put32(static_cast<uint32_t>(entry.interleaveEvery));
    put32(entry.effectiveFromUtc);
    put32(entry.effectiveToUtc);
    putString(entry.id, kMaxIdLength);
    putString(entry.assetId, kMaxAssetIdLength);
    putString(entry.text, kMaxTextLength);
    putString(entry.qrData, kMaxQrDataLength);
    putString(entry.location, kMaxLocationLength);
    ++entries_;
  }

  /// Stamps the entry count and the total length into the header. False when
  /// anything refused the blob, in which case the header is left without its
  /// count and length so no reader will take the partial bytes either.
  bool finish() {
    if (overLong_ || overflowed_ || entries_ > kMaxEntries || cursor_ > kMaxBlobBytes) {
      return false;
    }
    if (buffer_ != nullptr && capacity_ >= kHeaderBytes) {
      buffer_[3] = entries_;
      buffer_[4] = static_cast<uint8_t>(cursor_ & 0xFFu);
      buffer_[5] = static_cast<uint8_t>((cursor_ >> 8) & 0xFFu);
    }
    return true;
  }

  /// The bytes this policy needs, whether or not they fitted.
  uint16_t needed() const {
    return cursor_ > 0xFFFFu ? static_cast<uint16_t>(0xFFFFu) : static_cast<uint16_t>(cursor_);
  }

  uint8_t entries() const { return entries_; }

  /// True when the policy needed more bytes than the buffer held, or more than
  /// kMaxBlobBytes.
  bool overflowed() const { return overflowed_ || cursor_ > kMaxBlobBytes; }

  /// True when a string arrived past its bound, which is a different fault from
  /// an over-large policy and reads differently in the log.
  bool overLong() const { return overLong_; }

 private:
  bool fits(const char* text, size_t limit) const {
    return detail::boundedLength(text, limit + 1) <= limit;
  }

  void put8(uint8_t value) {
    if (buffer_ != nullptr && cursor_ < capacity_) {
      buffer_[cursor_] = value;
    } else if (buffer_ != nullptr) {
      overflowed_ = true;
    }
    ++cursor_;
  }

  void put16(uint16_t value) {
    put8(static_cast<uint8_t>(value & 0xFFu));
    put8(static_cast<uint8_t>((value >> 8) & 0xFFu));
  }

  void put32(uint32_t value) {
    put8(static_cast<uint8_t>(value & 0xFFu));
    put8(static_cast<uint8_t>((value >> 8) & 0xFFu));
    put8(static_cast<uint8_t>((value >> 16) & 0xFFu));
    put8(static_cast<uint8_t>((value >> 24) & 0xFFu));
  }

  void putString(const char* text, size_t limit) {
    const size_t length = detail::boundedLength(text, limit);
    for (size_t i = 0; i < length; ++i) {
      put8(static_cast<uint8_t>(text[i]));
    }
    put8(0);
  }

  uint8_t* buffer_ = nullptr;
  uint16_t capacity_ = 0;
  uint32_t cursor_ = 0;
  uint8_t entries_ = 0;
  bool overflowed_ = false;
  bool overLong_ = false;
};

/// Reads a blob back.
///
/// **begin() is the only place a blob is judged.** It validates the magic, the
/// format version, the declared length against the bytes actually present, the
/// entry count, every string's bound and that the entries end exactly where the
/// header said they would - and refuses the whole blob on any of those. So a
/// caller that got true from begin() can read every entry without checking
/// anything itself, and a blob that is wrong in any way leaves the device
/// waiting for the server, which is what it does today.
class Reader {
 public:
  bool begin(const uint8_t* blob, uint16_t length) {
    blob_ = nullptr;
    length_ = 0;
    entryCount_ = 0;
    read_ = 0;
    cursor_ = kHeaderBytes;

    if (blob == nullptr || length < kHeaderBytes) {
      return false;
    }
    if (blob[0] != kMagic0 || blob[1] != kMagic1 || blob[2] != kFormatVersion) {
      return false;
    }
    if (detail::read16(blob + 4) != length) {
      return false;
    }
    const uint8_t declared = blob[3];
    if (declared > kMaxEntries) {
      return false;
    }

    // The walk. Every entry has to be whole and every string terminated inside
    // the blob, and the last one has to end on the final byte - a blob with
    // bytes left over is not one this build wrote.
    uint16_t at = kHeaderBytes;
    for (uint8_t i = 0; i < declared; ++i) {
      if (!step(blob, length, at)) {
        return false;
      }
    }
    if (at != length) {
      return false;
    }

    blob_ = blob;
    length_ = length;
    entryCount_ = declared;
    return true;
  }

  uint8_t entryCount() const { return entryCount_; }

  int32_t defaultDwellSeconds() const {
    return blob_ == nullptr ? 0 : static_cast<int32_t>(detail::read32(blob_ + 6));
  }

  int32_t manualNavHoldSeconds() const {
    return blob_ == nullptr ? 0 : static_cast<int32_t>(detail::read32(blob_ + 10));
  }

  /// Fills `out` with the next entry. Its string members point into the blob, so
  /// they live exactly as long as the blob does.
  bool next(Entry& out) {
    if (blob_ == nullptr || read_ >= entryCount_) {
      return false;
    }
    const uint8_t* const at = blob_ + cursor_;
    out.kind = kindOf(at[0]);
    out.allowBanner = (at[1] & 0x01u) != 0;
    out.order = static_cast<int32_t>(detail::read32(at + 2));
    out.dwellSeconds = static_cast<int32_t>(detail::read32(at + 6));
    out.notableDwellSeconds = static_cast<int32_t>(detail::read32(at + 10));
    out.maxItems = static_cast<int32_t>(detail::read32(at + 14));
    out.interleaveEvery = static_cast<int32_t>(detail::read32(at + 18));
    out.effectiveFromUtc = detail::read32(at + 22);
    out.effectiveToUtc = detail::read32(at + 26);

    const char* text = reinterpret_cast<const char*>(at + kEntryFixedBytes);
    out.id = text;
    text = pastEnd(text);
    out.assetId = text;
    text = pastEnd(text);
    out.text = text;
    text = pastEnd(text);
    out.qrData = text;
    text = pastEnd(text);
    out.location = text;
    text = pastEnd(text);

    cursor_ = static_cast<uint16_t>(reinterpret_cast<const uint8_t*>(text) - blob_);
    ++read_;
    return true;
  }

  /// True once every declared entry has been read and the cursor sits on the
  /// final byte.
  bool complete() const {
    return blob_ != nullptr && read_ == entryCount_ && cursor_ == length_;
  }

 private:
  static KindCode kindOf(uint8_t code) {
    switch (code) {
      case static_cast<uint8_t>(KindCode::List):
        return KindCode::List;
      case static_cast<uint8_t>(KindCode::Interstitial):
        return KindCode::Interstitial;
      default:
        // A code a later format added. Unstated restores as an empty kind
        // string, which applyPolicy() reads as "leave the built-in kind alone".
        return KindCode::Unstated;
    }
  }

  static const char* pastEnd(const char* text) {
    while (*text != '\0') {
      ++text;
    }
    return text + 1;
  }

  /// Advances `at` past one whole entry, or returns false. The string bounds are
  /// checked here and not in next(), so next() has nothing left to refuse.
  static bool step(const uint8_t* blob, uint16_t length, uint16_t& at) {
    if (static_cast<uint32_t>(at) + kEntryFixedBytes > length) {
      return false;
    }
    uint32_t cursor = static_cast<uint32_t>(at) + kEntryFixedBytes;
    const uint16_t limits[kEntryStringCount] = {
        kMaxIdLength, kMaxAssetIdLength, kMaxTextLength, kMaxQrDataLength, kMaxLocationLength,
    };
    for (uint8_t s = 0; s < kEntryStringCount; ++s) {
      uint32_t scan = cursor;
      while (scan < length && blob[scan] != 0) {
        ++scan;
      }
      if (scan >= length) {
        // Ran off the end before finding a terminator.
        return false;
      }
      if (scan - cursor > limits[s]) {
        return false;
      }
      cursor = scan + 1;
    }
    at = static_cast<uint16_t>(cursor);
    return true;
  }

  const uint8_t* blob_ = nullptr;
  uint16_t length_ = 0;
  uint8_t entryCount_ = 0;
  uint8_t read_ = 0;
  uint16_t cursor_ = kHeaderBytes;
};

// ---- The NVS half. Defined in CardPolicyStore.cpp, which is where Preferences
// and Cards::Policy are named. Not reachable from a host test - what a bench run
// is still for is in TEST_PLAN.md section 1g.

/// How many bytes of stored policy are in flash, or 0 for none. Cheap: it reads
/// the blob's length and not the blob.
uint16_t storedBytes();

/// Reads the stored policy into `out`. False, and `out` left as a fresh Policy,
/// when there is none, when it belongs to a different enrollment, or when the
/// bytes are not a policy this build can read - and in the last two cases the
/// stored keys are removed, so the next boot does not try again.
bool load(Cards::Policy& out);

/// Stores a policy that just arrived from the server, if it differs from the one
/// already there. Writes nothing on an unchanged policy, which is the ordinary
/// case - see decideStore().
void save(const Cards::Policy& policy);

/// Removes the stored policy. The device then behaves on its next restart
/// exactly as it does today: it waits for the server.
void clear();

}  // namespace CardPolicyStore
