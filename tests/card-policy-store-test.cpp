// App/CardPolicyStore.h: the format a card policy is kept in across a restart,
// and the two decisions around it - whether a stored policy may be applied, and
// whether an arriving one is worth a flash write.
//
// The case this exists for is OUTAGE_CONTENT_LOSS_REVIEW.md section 1. Card
// policy lives in RAM and nowhere else, so any restart - power, the heap
// watchdog, a nudge at the wall - drops it, CardManager::draw() takes its guard
// clause, and the panel reads "Nothing to show yet" until the server answers
// again. The fleet restarts every 13 to 27 minutes under ordinary conditions
// (App.ino, measured 2026-09-11), so an outage longer than about a quarter of an
// hour empties most of the displays whether or not the unreachable watchdog ever
// fires.
//
// The header is compiled as it ships and names only <stdint.h> and <stddef.h>,
// so there are no stubs - the same reason App/UnreachableHoldOff.h and
// App/ReportedCounter.h give. The NVS half lives in App/CardPolicyStore.cpp and
// is not reachable from here; TEST_PLAN.md says what a bench run is still for.
//
// The two decision functions are asserted twice, as a static_assert over the
// constexpr function and again at runtime, so a widened predicate stops the
// compile on any toolchain and also prints a Failed! line under a runner.

#include <cassert>
#include <cstdio>
#include <cstring>

#include "CardPolicyStore.h"

namespace {

using CardPolicyStore::Decision;
using CardPolicyStore::Entry;
using CardPolicyStore::KindCode;
using CardPolicyStore::Reader;
using CardPolicyStore::Restore;
using CardPolicyStore::Writer;

int gFailures = 0;

void check(bool condition, const char* what) {
  if (!condition) {
    std::printf("Failed: %s\n", what);
    ++gFailures;
  }
}

// --- A STORED POLICY MUST NOT OVERRIDE A NEWER ONE FROM THE SERVER ---------
//
// The whole hazard of persisting this. A blob written an hour ago is older than
// anything the server has said since, so once a check-in has carried a policy
// the stored one is never consulted again - and it is applied at most once per
// boot, before the first check-in, so there is no second window for it to land
// in behind a fresher answer.

static_assert(CardPolicyStore::decideRestore(true, false, false) == Restore::Use,
              "A stored policy, no server answer yet, not yet restored: draw it.");
static_assert(CardPolicyStore::decideRestore(true, true, false) == Restore::ServerAlreadyAnswered,
              "The server has spoken this boot. The stored policy is the older statement and "
              "must not replace it.");
static_assert(CardPolicyStore::decideRestore(true, true, true) == Restore::ServerAlreadyAnswered,
              "The server's answer outranks the restore flag, whichever way that flag sits.");
static_assert(CardPolicyStore::decideRestore(true, false, true) == Restore::AlreadyRestored,
              "Once per boot. A second restore would re-apply a policy the rotation has "
              "already moved on from.");
static_assert(CardPolicyStore::decideRestore(false, false, false) == Restore::NothingStored,
              "Nothing stored - a device on its first boot, or one whose blob was refused. It "
              "waits for the server exactly as it does today.");

// --- A WRITE ONLY WHEN THE POLICY ACTUALLY CHANGED ------------------------
//
// ShouldSendCardPolicy on the server holds a device on what it has and resends
// at most hourly (DefaultCardPolicyResendMinutes), so an unchanged policy still
// arrives roughly 24 times a day. Writing on each of those is about 8,800 flash
// writes a year against a part rated near 100,000 - the same arithmetic
// Identity::noteHasPictureCards and Impressions' reserved id blocks already do.
// So the write is gated on the encoded bytes differing, not on a policy having
// arrived.

static_assert(CardPolicyStore::decideStore(true, 1566, true, 0xAABBCCDDu, 0xAABBCCDDu)
                  == Decision::Unchanged,
              "The same bytes are already in flash. This is the ordinary hourly case and it "
              "must not spend a write.");
static_assert(CardPolicyStore::decideStore(true, 1566, true, 0xAABBCCDDu, 0x11223344u)
                  == Decision::Write,
              "Different bytes - an operator edited the policy, or the cascade resolves "
              "somewhere else now.");
static_assert(CardPolicyStore::decideStore(true, 1566, false, 0xAABBCCDDu, 0xAABBCCDDu)
                  == Decision::Write,
              "Nothing recorded to compare against, so the match is meaningless.");
static_assert(CardPolicyStore::decideStore(false, 1566, true, 0xAABBCCDDu, 0x11223344u)
                  == Decision::NothingToStore,
              "No policy on this check-in means keep what is in force - including what is in "
              "flash. Absent must never be able to blank a restarted screen.");
static_assert(CardPolicyStore::decideStore(true, CardPolicyStore::kMaxBlobBytes, true,
                                           0xAABBCCDDu, 0x11223344u)
                  == Decision::Write,
              "Exactly the budget still fits.");
static_assert(CardPolicyStore::decideStore(true, CardPolicyStore::kMaxBlobBytes + 1, true,
                                           0xAABBCCDDu, 0x11223344u)
                  == Decision::TooLarge,
              "Past the budget. The stored blob is CLEARED rather than left alone - a kept "
              "one would resurrect the policy before this one on the next restart.");
static_assert(CardPolicyStore::decideStore(true, 0, true, 0xAABBCCDDu, 0x11223344u)
                  == Decision::TooLarge,
              "Zero bytes is an encoder that produced nothing, and the same clearing applies.");

// --- The codec ------------------------------------------------------------

Entry plainEntry(const char* id, int32_t order) {
  Entry entry;
  entry.id = id;
  entry.kind = KindCode::List;
  entry.order = order;
  entry.dwellSeconds = 12;
  entry.maxItems = 3;
  return entry;
}

/// The 42 ids this firmware registers, which is kMaxPolicyCards' worth and the
/// shape the sizing question in OUTAGE_CONTENT_LOSS_REVIEW.md asks about. Seven
/// sports ids were added on 2026-11-09 when Sports.cpp grew from four
/// registrations to eleven.
const char* const kEveryCardId[] = {
    "aircraft",      "calendar",      "clockdate",     "issflyover",   "listings",
    "moonphase",     "sunmoon",       "tides",         "homevalue",    "featuredlistings",
    "personalphotos", "graphic",      "graphic2",      "graphic3",     "graphic4",
    "graphic5",      "announcement",  "announcement2", "announcement3", "announcement4",
    "announcement5", "qrtext",        "qrtext2",       "qrtext3",      "qrtext4",
    "qrtext5",       "forecast",      "forecast2",     "forecast3",    "forecast4",
    "forecast5",     "sportsteam",    "sportsteam2",   "sportsscores", "sportslive",
    "sportsteam3",   "sportsteam4",   "sportsteam5",   "sportsscores2", "sportsscores3",
    "sportsscores4", "sportsscores5",
};
constexpr size_t kEveryCardCount = sizeof(kEveryCardId) / sizeof(kEveryCardId[0]);
static_assert(kEveryCardCount == CardPolicyStore::kMaxEntries,
              "The id list above is one entry per registered card, which is what Cards.h caps "
              "a policy at.");

uint8_t gBlob[CardPolicyStore::kMaxBlobBytes];

/// Fills a buffer with `length` repeats of 'x' plus a terminator.
void fill(char* out, size_t length, char c) {
  for (size_t i = 0; i < length; ++i) {
    out[i] = c;
  }
  out[length] = '\0';
}

void roundTripsOneEntry() {
  Entry in;
  in.id = "graphic3";
  in.kind = KindCode::Interstitial;
  in.order = -4;
  in.dwellSeconds = 18;
  in.notableDwellSeconds = 30;
  in.maxItems = 7;
  in.interleaveEvery = 5;
  in.allowBanner = true;
  in.effectiveFromUtc = 1762500000u;
  in.effectiveToUtc = 4000000000u;  // past a signed 32-bit second, which time_t is here
  in.assetId = "0123456789abcdef0123456789abcdef";
  in.text = "The pier is closed until Tuesday.";
  in.qrData = "https://discoveraroundme.com/cal";
  in.location = "target";

  Writer writer(gBlob, sizeof(gBlob));
  writer.begin(14, 45);
  writer.add(in);
  check(writer.finish(), "one entry encodes");
  check(!writer.overflowed(), "one entry does not overflow 4,096 bytes");

  Reader reader;
  check(reader.begin(gBlob, writer.needed()), "the blob it just wrote is accepted");
  check(reader.entryCount() == 1, "one entry declared");
  check(reader.defaultDwellSeconds() == 14, "defaultDwellSeconds survives");
  check(reader.manualNavHoldSeconds() == 45, "manualNavHoldSeconds survives");

  Entry out;
  check(reader.next(out), "the entry reads back");
  check(std::strcmp(out.id, in.id) == 0, "id survives");
  check(out.kind == in.kind, "kind survives");
  check(out.order == in.order, "a negative order survives");
  check(out.dwellSeconds == in.dwellSeconds, "dwellSeconds survives");
  check(out.notableDwellSeconds == in.notableDwellSeconds, "notableDwellSeconds survives");
  check(out.maxItems == in.maxItems, "maxItems survives");
  check(out.interleaveEvery == in.interleaveEvery, "interleaveEvery survives");
  check(out.allowBanner == in.allowBanner, "allowBanner survives");
  check(out.effectiveFromUtc == in.effectiveFromUtc, "effectiveFromUtc survives");
  check(out.effectiveToUtc == in.effectiveToUtc, "an effectiveToUtc past 2038 survives");
  check(std::strcmp(out.assetId, in.assetId) == 0, "assetId survives");
  check(std::strcmp(out.text, in.text) == 0, "text survives");
  check(std::strcmp(out.qrData, in.qrData) == 0, "qrData survives");
  check(std::strcmp(out.location, in.location) == 0, "location survives");
  check(!reader.next(out), "there is no second entry");
  check(reader.complete(), "the blob ended exactly where the header said it would");
}

/// Absent on the wire arrives as an empty String and must come back empty, not
/// as a stray value from the entry beside it. Every card but a handful carries
/// four empty strings, so this is the ordinary entry rather than an edge case.
void roundTripsEmptyStrings() {
  Entry in;
  in.id = "clockdate";
  in.kind = KindCode::Unstated;

  Writer writer(gBlob, sizeof(gBlob));
  writer.begin(0, 0);
  writer.add(in);
  check(writer.finish(), "an entry with nothing but an id encodes");

  Reader reader;
  check(reader.begin(gBlob, writer.needed()), "it is accepted");
  Entry out;
  check(reader.next(out), "it reads back");
  check(std::strcmp(out.id, "clockdate") == 0, "the id is there");
  check(out.kind == KindCode::Unstated,
        "an unrecognised kind restores as unstated, which applyPolicy() reads as 'leave the "
        "card's built-in kind alone' - the same thing the original string did");
  check(out.assetId[0] == '\0', "assetId comes back empty");
  check(out.text[0] == '\0', "text comes back empty");
  check(out.qrData[0] == '\0', "qrData comes back empty");
  check(out.location[0] == '\0', "location comes back empty");
}

/// THE SIZING ANSWER. A full 35-card policy carrying no content strings, which
/// is what the review asked for. The number is asserted rather than printed
/// because it is what the 4,096-byte budget is justified against.
void fullPolicyFitsTheBudget() {
  Writer measure(nullptr, 0);
  measure.begin(12, 30);
  for (size_t i = 0; i < kEveryCardCount; ++i) {
    measure.add(plainEntry(kEveryCardId[i], static_cast<int32_t>(i)));
  }
  measure.finish();
  check(measure.entries() == CardPolicyStore::kMaxEntries, "all 42 entries measured");
  check(measure.needed() == 1896,
        "a 42-card policy with no content strings is 1,896 bytes. If this number moved, the "
        "format changed and kFormatVersion has to move with it. It was 1,566 at 35 entries, "
        "before Sports.cpp registered its other seven sports ids on 2026-11-09.");

  Writer writer(gBlob, sizeof(gBlob));
  writer.begin(12, 30);
  for (size_t i = 0; i < kEveryCardCount; ++i) {
    writer.add(plainEntry(kEveryCardId[i], static_cast<int32_t>(i)));
  }
  check(writer.finish(), "and it encodes");
  check(writer.needed() == measure.needed(), "the measuring pass agreed with the real one");

  Reader reader;
  check(reader.begin(gBlob, writer.needed()), "the 35-card blob is accepted");
  check(reader.entryCount() == CardPolicyStore::kMaxEntries, "35 entries declared");
  size_t read = 0;
  Entry out;
  while (reader.next(out)) {
    check(std::strcmp(out.id, kEveryCardId[read]) == 0, "entries come back in policy order");
    check(out.order == static_cast<int32_t>(read), "each entry keeps its own order");
    ++read;
  }
  check(read == kEveryCardCount, "all 35 read back");
  check(reader.complete(), "nothing left over");
}

/// The realistic worst case an operator can build: every content field filled to
/// the bound on the five instances that have one. This is the figure the budget
/// has to clear, and it does.
void realisticWorstCaseFitsTheBudget() {
  char text[CardPolicyStore::kMaxTextLength + 1];
  char qr[CardPolicyStore::kMaxQrDataLength + 1];
  char asset[CardPolicyStore::kMaxAssetIdLength + 1];
  fill(text, CardPolicyStore::kMaxTextLength, 'a');
  fill(qr, CardPolicyStore::kMaxQrDataLength, 'q');
  fill(asset, CardPolicyStore::kMaxAssetIdLength, 'f');

  Writer measure(nullptr, 0);
  measure.begin(12, 30);
  for (size_t i = 0; i < kEveryCardCount; ++i) {
    Entry entry = plainEntry(kEveryCardId[i], static_cast<int32_t>(i));
    const char* const id = kEveryCardId[i];
    if (std::strncmp(id, "announcement", 12) == 0) {
      entry.text = text;
    } else if (std::strncmp(id, "qrtext", 6) == 0) {
      entry.qrData = qr;
    } else if (std::strncmp(id, "graphic", 7) == 0) {
      entry.assetId = asset;
    } else if (std::strncmp(id, "forecast", 8) == 0) {
      entry.location = "target";
    }
    measure.add(entry);
  }
  measure.finish();
  check(!measure.overflowed(),
        "five full announcements, five full QR payloads, five asset ids and five locations on "
        "top of all 35 cards still fit the budget");
  check(measure.needed() < CardPolicyStore::kMaxBlobBytes,
        "and with room left - this is the figure the 4,096 is chosen against");
}

/// The theoretical worst case does NOT fit, and that is the point of the budget.
/// A policy this large is refused and the device behaves exactly as it does
/// today: it waits for the server.
void theoreticalWorstCaseIsRefused() {
  char text[CardPolicyStore::kMaxTextLength + 1];
  char qr[CardPolicyStore::kMaxQrDataLength + 1];
  char asset[CardPolicyStore::kMaxAssetIdLength + 1];
  char location[CardPolicyStore::kMaxLocationLength + 1];
  fill(text, CardPolicyStore::kMaxTextLength, 'a');
  fill(qr, CardPolicyStore::kMaxQrDataLength, 'q');
  fill(asset, CardPolicyStore::kMaxAssetIdLength, 'f');
  fill(location, CardPolicyStore::kMaxLocationLength, 'l');

  Writer measure(nullptr, 0);
  measure.begin(12, 30);
  for (size_t i = 0; i < kEveryCardCount; ++i) {
    Entry entry = plainEntry(kEveryCardId[i], static_cast<int32_t>(i));
    entry.assetId = asset;
    entry.text = text;
    entry.qrData = qr;
    entry.location = location;
    measure.add(entry);
  }
  measure.finish();
  check(measure.overflowed(),
        "every field on every card filled to its bound is past the budget, which is what "
        "decideStore()'s TooLarge branch is for");
  check(CardPolicyStore::decideStore(true, measure.needed(), true, 1u, 2u) == Decision::TooLarge,
        "and the decision agrees");
}

/// A writer handed a buffer too small writes nothing past the end and says so,
/// rather than producing a short blob a reader would accept.
void overflowIsReported() {
  uint8_t small[64];
  const uint8_t canary = 0x5A;
  std::memset(small, canary, sizeof(small));

  Writer writer(small, sizeof(small));
  writer.begin(12, 30);
  for (size_t i = 0; i < kEveryCardCount; ++i) {
    writer.add(plainEntry(kEveryCardId[i], static_cast<int32_t>(i)));
  }
  check(!writer.finish(), "finish() refuses an overflowed blob");
  check(writer.overflowed(), "and says it overflowed");
  check(writer.needed() == 1896, "while still reporting what it would have needed");

  Reader reader;
  check(!reader.begin(small, sizeof(small)),
        "the partial bytes are not a blob a reader will take");
}

/// Every reason a stored blob is refused. Each one leaves the device waiting for
/// the server, which is today's behaviour and is always safe.
void malformedBlobsAreRefused() {
  Writer writer(gBlob, sizeof(gBlob));
  writer.begin(12, 30);
  writer.add(plainEntry("tides", 1));
  writer.add(plainEntry("sunmoon", 2));
  check(writer.finish(), "a two-entry blob encodes");
  const uint16_t length = writer.needed();

  uint8_t copy[256];
  check(length <= sizeof(copy), "the two-entry blob fits the scratch copy");

  {
    Reader reader;
    std::memcpy(copy, gBlob, length);
    check(reader.begin(copy, length), "the untouched copy is accepted");
  }
  {
    Reader reader;
    std::memcpy(copy, gBlob, length);
    copy[0] = 'X';
    check(!reader.begin(copy, length), "a wrong magic byte is refused");
  }
  {
    Reader reader;
    std::memcpy(copy, gBlob, length);
    copy[2] = CardPolicyStore::kFormatVersion + 1;
    check(!reader.begin(copy, length),
          "a FORMAT VERSION this build does not know is refused. This is what stops a blob "
          "written by a later firmware being read as if it were this one.");
  }
  {
    Reader reader;
    std::memcpy(copy, gBlob, length);
    copy[2] = CardPolicyStore::kFormatVersion - 1;
    check(!reader.begin(copy, length), "and an older format version is refused too");
  }
  {
    Reader reader;
    std::memcpy(copy, gBlob, length);
    check(!reader.begin(copy, static_cast<uint16_t>(length - 1)),
          "a blob one byte shorter than its header declares is refused");
  }
  {
    Reader reader;
    std::memcpy(copy, gBlob, length);
    copy[3] = 3;  // entryCount
    check(!reader.begin(copy, length),
          "an entry count that disagrees with the bytes present is refused");
  }
  {
    Reader reader;
    check(!reader.begin(nullptr, length), "no blob at all is refused");
  }
  {
    Reader reader;
    check(!reader.begin(copy, 0), "a zero-length blob is refused");
  }
  {
    Reader reader;
    check(!reader.begin(copy, CardPolicyStore::kHeaderBytes - 1),
          "fewer bytes than a header is refused");
  }
}

/// An over-long string is refused rather than truncated, which is Cards.h's
/// standing rule for asset ids: a truncated id is a perfectly well-formed id for
/// some OTHER asset, so drawing the wrong picture is worse than drawing none.
void overLongStringsAreRefused() {
  char tooLong[CardPolicyStore::kMaxAssetIdLength + 2];
  fill(tooLong, CardPolicyStore::kMaxAssetIdLength + 1, 'f');

  Entry in = plainEntry("graphic", 1);
  in.assetId = tooLong;

  Writer writer(gBlob, sizeof(gBlob));
  writer.begin(12, 30);
  writer.add(in);
  check(!writer.finish(),
        "an assetId one character past the bound is refused at the encoder, not shortened");
  check(writer.overLong(), "and the writer says which kind of refusal it was");
}

/// A policy that drops a card must not leave that card behind in flash. The blob
/// is rewritten whole, so the dropped card is simply not in it.
void aLaterPolicyDoesNotResurrectADroppedCard() {
  Writer first(gBlob, sizeof(gBlob));
  first.begin(12, 30);
  first.add(plainEntry("tides", 1));
  first.add(plainEntry("aircraft", 2));
  first.add(plainEntry("listings", 3));
  check(first.finish(), "the three-card policy encodes");
  const uint32_t firstHash = CardPolicyStore::hash(gBlob, first.needed());

  Writer second(gBlob, sizeof(gBlob));
  second.begin(12, 30);
  second.add(plainEntry("tides", 1));
  second.add(plainEntry("listings", 3));
  check(second.finish(), "the two-card policy that replaces it encodes");
  const uint32_t secondHash = CardPolicyStore::hash(gBlob, second.needed());

  check(firstHash != secondHash,
        "dropping a card changes the bytes, so decideStore() writes rather than skipping");
  check(CardPolicyStore::decideStore(true, second.needed(), true, secondHash, firstHash)
            == Decision::Write,
        "and the decision agrees");

  Reader reader;
  check(reader.begin(gBlob, second.needed()), "the replacement is accepted");
  check(reader.entryCount() == 2, "it has two entries");
  Entry out;
  bool sawAircraft = false;
  while (reader.next(out)) {
    if (std::strcmp(out.id, "aircraft") == 0) {
      sawAircraft = true;
    }
  }
  check(!sawAircraft, "the dropped card is gone from the stored policy");
}

/// The same bytes hash the same and different bytes do not, because that is the
/// whole of the flash-wear gate.
void hashingDistinguishesPolicies() {
  Writer a(gBlob, sizeof(gBlob));
  a.begin(12, 30);
  a.add(plainEntry("tides", 1));
  check(a.finish(), "policy A encodes");
  const uint32_t hashA = CardPolicyStore::hash(gBlob, a.needed());

  Writer again(gBlob, sizeof(gBlob));
  again.begin(12, 30);
  again.add(plainEntry("tides", 1));
  check(again.finish(), "the identical policy encodes");
  check(CardPolicyStore::hash(gBlob, again.needed()) == hashA,
        "an unchanged policy hashes the same, so the hourly resend spends no write");

  Writer dwell(gBlob, sizeof(gBlob));
  dwell.begin(13, 30);  // one second of default dwell
  dwell.add(plainEntry("tides", 1));
  check(dwell.finish(), "the policy with a different default dwell encodes");
  check(CardPolicyStore::hash(gBlob, dwell.needed()) != hashA,
        "a one-second change to the default dwell is a change");

  Writer banner(gBlob, sizeof(gBlob));
  banner.begin(12, 30);
  Entry withBanner = plainEntry("tides", 1);
  withBanner.allowBanner = true;
  banner.add(withBanner);
  check(banner.finish(), "the policy with banners allowed encodes");
  check(CardPolicyStore::hash(gBlob, banner.needed()) != hashA,
        "flipping allowBanner is a change");
}

struct RestoreCase {
  bool haveStored;
  bool serverApplied;
  bool alreadyRestored;
  Restore expected;
  const char* what;
};

constexpr RestoreCase kRestoreCases[] = {
    {true, false, false, Restore::Use, "stored, no server answer, first restore"},
    {true, true, false, Restore::ServerAlreadyAnswered, "the server already answered"},
    {true, true, true, Restore::ServerAlreadyAnswered, "the server answer outranks the flag"},
    {true, false, true, Restore::AlreadyRestored, "already restored this boot"},
    {false, false, false, Restore::NothingStored, "nothing stored"},
    {false, true, false, Restore::ServerAlreadyAnswered, "nothing stored and no need of it"},
};

struct StoreCase {
  bool present;
  uint16_t bytes;
  bool haveStoredHash;
  uint32_t encoded;
  uint32_t stored;
  Decision expected;
  const char* what;
};

constexpr StoreCase kStoreCases[] = {
    {true, 1566, true, 7u, 7u, Decision::Unchanged, "the hourly resend of an unchanged policy"},
    {true, 1566, true, 7u, 8u, Decision::Write, "an edited policy"},
    {true, 1566, false, 7u, 7u, Decision::Write, "nothing recorded to compare against"},
    {false, 1566, true, 7u, 8u, Decision::NothingToStore, "no policy on this check-in"},
    {true, CardPolicyStore::kMaxBlobBytes, true, 7u, 8u, Decision::Write, "exactly the budget"},
    {true, CardPolicyStore::kMaxBlobBytes + 1, true, 7u, 8u, Decision::TooLarge, "past it"},
    {true, 0, true, 7u, 8u, Decision::TooLarge, "nothing encoded"},
};

const char* name(Restore value) {
  switch (value) {
    case Restore::Use:
      return "Use";
    case Restore::NothingStored:
      return "NothingStored";
    case Restore::ServerAlreadyAnswered:
      return "ServerAlreadyAnswered";
    case Restore::AlreadyRestored:
      return "AlreadyRestored";
  }
  return "an unnamed Restore";
}

const char* name(Decision value) {
  switch (value) {
    case Decision::Write:
      return "Write";
    case Decision::Unchanged:
      return "Unchanged";
    case Decision::NothingToStore:
      return "NothingToStore";
    case Decision::TooLarge:
      return "TooLarge";
  }
  return "an unnamed Decision";
}

}  // namespace

int main() {
  for (const auto& c : kRestoreCases) {
    const Restore got =
        CardPolicyStore::decideRestore(c.haveStored, c.serverApplied, c.alreadyRestored);
    if (got != c.expected) {
      std::printf("Failed: %s - expected %s, got %s\n", c.what, name(c.expected), name(got));
      ++gFailures;
    }
  }

  for (const auto& c : kStoreCases) {
    const Decision got =
        CardPolicyStore::decideStore(c.present, c.bytes, c.haveStoredHash, c.encoded, c.stored);
    if (got != c.expected) {
      std::printf("Failed: %s - expected %s, got %s\n", c.what, name(c.expected), name(got));
      ++gFailures;
    }
  }

  roundTripsOneEntry();
  roundTripsEmptyStrings();
  fullPolicyFitsTheBudget();
  realisticWorstCaseFitsTheBudget();
  theoreticalWorstCaseIsRefused();
  overflowIsReported();
  malformedBlobsAreRefused();
  overLongStringsAreRefused();
  aLaterPolicyDoesNotResurrectADroppedCard();
  hashingDistinguishesPolicies();

  const size_t cases = (sizeof(kRestoreCases) / sizeof(kRestoreCases[0]))
                       + (sizeof(kStoreCases) / sizeof(kStoreCases[0])) + 10;

  if (gFailures != 0) {
    std::printf("Failed: %d check(s) over the card policy store\n", gFailures);
    return 1;
  }

  std::printf("PASS: card policy store, %zu cases\n", cases);
  return 0;
}
