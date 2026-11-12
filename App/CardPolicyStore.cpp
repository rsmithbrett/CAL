#include "CardPolicyStore.h"

#include <Arduino.h>
#include <Preferences.h>

#include <new>

#include "Cards.h"
#include "Identity.h"
#include "Log.h"

namespace CardPolicyStore {
namespace {

// Its own namespace rather than a corner of Identity's "cal", the same way
// Impressions keeps "impr" and BootDiag keeps "bootdiag". NVS keys are capped at
// 15 characters and truncate past that without saying so, which is why these
// stay short.
constexpr const char* kNvsNamespace = "cardpol";

constexpr const char* kKeyBlob = "blob";
constexpr const char* kKeyHash = "hash";
constexpr const char* kKeyOwner = "owner";

/// The bounds in the header are a copy of the ones in Cards.h, because the
/// header has to compile with nothing but <stdint.h> for the host test. These
/// are what stop the two drifting: a change to either side is a compile error
/// here rather than a policy quietly stored short or an id truncated into a
/// perfectly valid id for some other asset.
static_assert(kMaxEntries == Cards::kMaxPolicyCards,
              "CardPolicyStore::kMaxEntries and Cards::kMaxPolicyCards must agree.");
static_assert(kMaxAssetIdLength == Cards::kMaxAssetIdLength,
              "CardPolicyStore::kMaxAssetIdLength and Cards::kMaxAssetIdLength must agree.");
static_assert(kMaxTextLength == Cards::kMaxTextLength,
              "CardPolicyStore::kMaxTextLength and Cards::kMaxTextLength must agree.");
static_assert(kMaxQrDataLength == Cards::kMaxQrDataLength,
              "CardPolicyStore::kMaxQrDataLength and Cards::kMaxQrDataLength must agree.");
static_assert(kMaxLocationLength == Cards::kMaxLocationLength,
              "CardPolicyStore::kMaxLocationLength and Cards::kMaxLocationLength must agree.");
static_assert(kMaxIdLength == Cards::kMaxAnnouncementCardIdLength,
              "The stored id bound tracks the one Cards.h already sizes a card id against.");

KindCode kindOf(const String& kind) {
  if (kind == "list") {
    return KindCode::List;
  }
  if (kind == "interstitial") {
    return KindCode::Interstitial;
  }
  // Anything else, including an absent field, leaves the card's built-in kind
  // alone - see CardManager::applyPolicy(), which only assigns on an exact
  // match. Unstated restores as an empty string and does the same thing.
  return KindCode::Unstated;
}

const char* kindName(KindCode kind) {
  switch (kind) {
    case KindCode::List:
      return "list";
    case KindCode::Interstitial:
      return "interstitial";
    case KindCode::Unstated:
      break;
  }
  return "";
}

/// A string that is over its bound is stored EMPTY, not shortened - and that is
/// what CardManager::applyPolicy() does with the same value, so the stored
/// policy reproduces what is on the glass rather than a tidier version of it.
/// Reported once per field per policy for the same reason applyPolicy reports
/// it: a bound the server and this firmware disagree about should not be a
/// silent difference.
///
/// `report` is false on the measuring pass, which encodes the same policy a
/// second time - one line per over-long field, not two.
const char* bounded(const String& value, size_t limit, const char* field, const char* cardId,
                    bool report) {
  if (value.length() > limit) {
    if (report) {
      Log::printf("[cardpol] not storing %s for '%s' - %u characters against the %u this device "
                  "holds, and applyPolicy dropped it too",
                  field, cardId, static_cast<unsigned>(value.length()),
                  static_cast<unsigned>(limit));
    }
    return "";
  }
  return value.c_str();
}

void encode(Writer& writer, const Cards::Policy& policy, bool report) {
  writer.begin(policy.defaultDwellSeconds, policy.manualNavHoldSeconds);
  for (uint8_t i = 0; i < policy.entryCount; ++i) {
    const Cards::PolicyEntry& source = policy.entries[i];
    const char* const cardId = source.id.c_str();

    Entry entry;
    entry.id = bounded(source.id, kMaxIdLength, "the id", cardId, report);
    entry.kind = kindOf(source.kind);
    entry.order = source.order;
    entry.dwellSeconds = source.dwellSeconds;
    entry.notableDwellSeconds = source.notableDwellSeconds;
    entry.maxItems = source.maxItems;
    entry.interleaveEvery = source.interleaveEvery;
    entry.allowBanner = source.allowBanner;
    entry.effectiveFromUtc = static_cast<uint32_t>(source.effectiveFromUtc);
    entry.effectiveToUtc = static_cast<uint32_t>(source.effectiveToUtc);
    entry.assetId = bounded(source.assetId, kMaxAssetIdLength, "the assetId", cardId, report);
    entry.text = bounded(source.text, kMaxTextLength, "the text", cardId, report);
    entry.qrData = bounded(source.qrData, kMaxQrDataLength, "the qrData", cardId, report);
    entry.location = bounded(source.location, kMaxLocationLength, "the location", cardId, report);
    writer.add(entry);
  }
}

/// Which enrolled device these bytes were written by.
///
/// A policy is one household's rotation, so a unit that has been handed a
/// different identity must not draw the last one's. CAL's touch recovery clears
/// the device secret and leaves the rest of NVS standing (Recovery.cpp), so the
/// blob outlives a de-enrollment unless something ties the two together. Four
/// bytes of the secret's hash is that tie: a device that re-enrolls under a new
/// secret finds the stamp disagreeing and starts from nothing. A wired install
/// erases NVS outright and takes the blob with it either way.
uint32_t ownerStamp() {
  const String secret = Identity::deviceSecret();
  return hash(reinterpret_cast<const uint8_t*>(secret.c_str()),
              static_cast<uint16_t>(secret.length()));
}

void forget(Preferences& prefs) {
  prefs.remove(kKeyBlob);
  prefs.remove(kKeyHash);
  prefs.remove(kKeyOwner);
}

}  // namespace

void clear() {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, /*readOnly=*/false)) {
    return;
  }
  forget(prefs);
  prefs.end();
}

uint16_t storedBytes() {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, /*readOnly=*/true)) {
    return 0;
  }
  const size_t length = prefs.getBytesLength(kKeyBlob);
  prefs.end();
  return length > kMaxBlobBytes ? 0 : static_cast<uint16_t>(length);
}

bool load(Cards::Policy& out) {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, /*readOnly=*/true)) {
    Log::line("[cardpol] the stored-policy namespace would not open");
    return false;
  }
  const size_t length = prefs.getBytesLength(kKeyBlob);
  const uint32_t storedOwner = prefs.getUInt(kKeyOwner, 0);
  if (length == 0 || length > kMaxBlobBytes) {
    prefs.end();
    return false;
  }
  if (storedOwner != ownerStamp()) {
    prefs.end();
    Log::line("[cardpol] the stored policy belongs to a different enrollment - discarded");
    clear();
    return false;
  }

  uint8_t* const blob = new (std::nothrow) uint8_t[length];
  if (blob == nullptr) {
    prefs.end();
    Log::printf("[cardpol] no room for the %u bytes of stored policy - waiting for the server",
                static_cast<unsigned>(length));
    return false;
  }
  const size_t read = prefs.getBytes(kKeyBlob, blob, length);
  prefs.end();

  bool ok = false;
  // Whether the bytes themselves are the problem, which is the only reason to
  // remove them. A short read is the partition having a bad moment and says
  // nothing about what is written there, so that one leaves the blob alone and
  // the next boot tries again.
  bool discard = false;
  Reader reader;
  if (read != length) {
    Log::printf("[cardpol] the stored policy read %u of %u bytes - not applied, and left where "
                "it is",
                static_cast<unsigned>(read), static_cast<unsigned>(length));
  } else if (!reader.begin(blob, static_cast<uint16_t>(length))) {
    // Magic, format version, declared length, entry count or a string bound.
    // Whichever it was, the bytes are not a policy this build wrote, and a
    // guess at them would restore a rotation nobody configured.
    Log::printf("[cardpol] the %u stored bytes are not a policy this build can read - discarded",
                static_cast<unsigned>(length));
    discard = true;
  } else {
    out.present = true;
    out.defaultDwellSeconds = reader.defaultDwellSeconds();
    out.manualNavHoldSeconds = reader.manualNavHoldSeconds();
    out.entryCount = 0;

    Entry entry;
    // The cap is tested FIRST. next() consumes an entry whether or not the
    // caller keeps it, so the other order would read one and drop it, and
    // complete() below would then be answering about a policy that was not
    // fully applied. Reader::begin() already refuses a blob declaring more than
    // kMaxEntries, so this is a guard rather than a path.
    while (out.entryCount < Cards::kMaxPolicyCards && reader.next(entry)) {
      Cards::PolicyEntry& target = out.entries[out.entryCount++];
      target.id = String(entry.id);
      target.kind = String(kindName(entry.kind));
      target.order = entry.order;
      target.dwellSeconds = entry.dwellSeconds;
      target.notableDwellSeconds = entry.notableDwellSeconds;
      target.maxItems = entry.maxItems;
      target.interleaveEvery = entry.interleaveEvery;
      target.allowBanner = entry.allowBanner;
      target.effectiveFromUtc = static_cast<time_t>(entry.effectiveFromUtc);
      target.effectiveToUtc = static_cast<time_t>(entry.effectiveToUtc);
      target.assetId = String(entry.assetId);
      target.text = String(entry.text);
      target.qrData = String(entry.qrData);
      target.location = String(entry.location);
    }
    ok = reader.complete();
    if (!ok) {
      Log::printf("[cardpol] the stored policy declared %u entries and %u were read - discarded",
                  static_cast<unsigned>(reader.entryCount()),
                  static_cast<unsigned>(out.entryCount));
      discard = true;
    }
  }

  delete[] blob;
  if (!ok) {
    // Left in a state nothing will act on. Not assigned a fresh Cards::Policy()
    // - that would put a second one of these, the better part of five
    // kilobytes, on the stack to construct the temporary.
    out.present = false;
    out.entryCount = 0;
    if (discard) {
      clear();
    }
  }
  return ok;
}

void save(const Cards::Policy& policy) {
  // The measuring pass. A Writer with no buffer counts the bytes and writes
  // none, which is how the one allocation below is sized to what this policy
  // actually needs instead of reserving kMaxBlobBytes it almost never uses.
  Writer measure(nullptr, 0);
  encode(measure, policy, /*report=*/false);
  const bool encodable = measure.finish();
  const uint16_t needed = measure.needed();

  // decideStore reads the size before it reads either hash, so asking it here,
  // with the hashes still unknown, settles the two answers that need no buffer.
  Decision decision = decideStore(policy.present, needed, false, 0, 0);
  if (encodable && decision == Decision::Write) {
    uint8_t* const blob = new (std::nothrow) uint8_t[needed];
    if (blob == nullptr) {
      Log::printf("[cardpol] no room to encode %u bytes of policy - this one is not stored",
                  static_cast<unsigned>(needed));
      return;
    }
    Writer writer(blob, needed);
    encode(writer, policy, /*report=*/true);
    if (!writer.finish()) {
      // The measuring pass and the real one disagreed, which they cannot do over
      // the same policy. Said out loud rather than written.
      delete[] blob;
      Log::line("[cardpol] the policy encoded differently the second time - not stored");
      return;
    }

    const uint32_t encodedHash = hash(blob, needed);
    Preferences prefs;
    if (!prefs.begin(kNvsNamespace, /*readOnly=*/false)) {
      delete[] blob;
      Log::line("[cardpol] the stored-policy namespace would not open for writing");
      return;
    }
    // Both keys, not just the blob. A blob with no hash beside it cannot be
    // compared against, and reading a missing hash as 0 would make an encoded
    // hash of 0 look like a match and skip a write that was needed.
    const bool haveStoredHash = prefs.isKey(kKeyHash) && prefs.getBytesLength(kKeyBlob) > 0;
    const uint32_t storedHash = prefs.getUInt(kKeyHash, 0);
    decision = decideStore(policy.present, needed, haveStoredHash, encodedHash, storedHash);

    if (decision == Decision::Write) {
      const size_t written = prefs.putBytes(kKeyBlob, blob, needed);
      if (written == needed) {
        prefs.putUInt(kKeyHash, encodedHash);
        prefs.putUInt(kKeyOwner, ownerStamp());
        Log::printf("[cardpol] policy stored - %u entries in %u bytes, so a restart draws this "
                    "rotation with no server",
                    static_cast<unsigned>(policy.entryCount), static_cast<unsigned>(needed));
      } else {
        // A partial blob and a hash naming the whole one would read as valid
        // next boot. Both keys go instead.
        forget(prefs);
        Log::printf("[cardpol] only %u of %u bytes were stored - the stored policy is cleared",
                    static_cast<unsigned>(written), static_cast<unsigned>(needed));
      }
    } else {
      Log::verbose("[cardpol] this policy is already the stored one - no write");
    }
    prefs.end();
    delete[] blob;
    return;
  }

  switch (decision) {
    case Decision::NothingToStore:
      // applyPolicy() returns before reaching here on an absent policy, so this
      // is a guard rather than a path. Absent means keep what is in force, and
      // what is in flash is part of that.
      break;
    case Decision::TooLarge:
      Log::printf("[cardpol] this policy needs %u bytes and the budget is %u - the stored policy "
                  "is cleared, so a restart waits for the server rather than drawing an older "
                  "rotation",
                  static_cast<unsigned>(needed), static_cast<unsigned>(kMaxBlobBytes));
      clear();
      break;
    case Decision::Write:
    case Decision::Unchanged:
      if (!encodable) {
        Log::line("[cardpol] this policy would not encode - the stored policy is cleared");
        clear();
      }
      break;
  }
}

}  // namespace CardPolicyStore
