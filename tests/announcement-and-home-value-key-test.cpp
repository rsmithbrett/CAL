// App/HomeValue.cpp and App/Announcement.cpp: the content key each draw reports,
// and the conditions that decide whether it reports one at all.
//
// Both production files are compiled as they ship. Cards, Display and Log are
// declarations that parse on a host once Arduino.h is stubbed, and the functions
// the two modules call into are defined below. The Cards stub keeps every
// descriptor that registers, which is the only way to reach a resolver sitting in
// an anonymous namespace - CardManager::noteShowing() reaches them the same way,
// through CardSpec::listingId. It also serves Cards::indexOf() and Cards::at(),
// which is how Announcement.cpp reads the text its own policy entry carries.
//
// What the cases are here for is CARD_IMPRESSIONS_DESIGN.md in the server tree,
// "What a home-value and an announcement draw report", and the content-key table
// in BOOT_BUTTON_AND_IMPRESSIONS_DESIGN.md beside this. The server keys a
// quarter-hour period row on the content key, so these six cards reported a
// summary on every draw and collapsed every address and every notice into one row
// per quarter hour.
//
// The two keys are different shapes because the two cards hold different things.
// A home-value draw is about one property and the check-in response carries its
// address, so the address is the key. An announcement holds its text and nothing
// else, and that text runs to 280 characters against a 64-character key, so the
// key is a digest of it.

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "Announcement.h"
#include "Cards.h"
#include "Display.h"
#include "HomeValue.h"
#include "Log.h"

namespace {

// --- the registry the two modules register into -----------------------------

/// Every descriptor that registered, in a function-local static so nothing
/// depends on the order translation units initialise in. Both modules register at
/// static-init time, before main() runs.
///
/// Reserved up front and never grown past that, because Cards::at() hands back a
/// reference and a reallocation would invalidate one.
std::vector<Cards::CardSpec>& registry() {
  static std::vector<Cards::CardSpec>* held = [] {
    auto* fresh = new std::vector<Cards::CardSpec>();
    fresh->reserve(32);
    return fresh;
  }();
  return *held;
}

Cards::CardSpec* findCard(const char* id) {
  for (auto& spec : registry()) {
    if (std::strcmp(spec.id, id) == 0) {
      return &spec;
    }
  }
  return nullptr;
}

/// The key the registered resolver answers for one item of one card.
std::string keyFor(const char* cardId, uint16_t item) {
  Cards::CardSpec* const spec = findCard(cardId);
  assert(spec != nullptr && "no card registered under that id");
  assert(spec->listingId != nullptr && "that card registers no listingId resolver");
  return spec->listingId(item);
}

bool hasResolver(const char* cardId) {
  Cards::CardSpec* const spec = findCard(cardId);
  return spec != nullptr && spec->listingId != nullptr;
}

/// Writes text onto an announcement instance's own descriptor, which is what
/// CardManager::applyPolicy() does when a policy names that id.
void setNoticeText(const char* cardId, const std::string& text) {
  Cards::CardSpec* const spec = findCard(cardId);
  assert(spec != nullptr && "no announcement registered under that id");
  std::strncpy(spec->text, text.c_str(), Cards::kMaxTextLength);
  spec->text[Cards::kMaxTextLength] = '\0';
}

/// The five ids Announcement.cpp registers. There is no "announcement1" - the
/// first slot is plain "announcement", and KnownCards.cs on the server agrees.
const char* const kNoticeIds[5] = {"announcement", "announcement2", "announcement3",
                                   "announcement4", "announcement5"};

/// A home value with everything the server sends filled in.
void setHomeValue(int estimate, const char* address) {
  HomeValue::setValue(estimate, estimate - 40000, estimate + 40000, 312.5, 1762000000,
                      String(address));
}

// --- home value -------------------------------------------------------------

/// The card declares a resolver at all.
///
/// This is the case the card shipped failing: its address rides the check-in
/// response rather than the card policy, so the assetId and qrData fallbacks
/// CardManager::noteShowing() applies after the resolver are both empty, and with
/// no resolver every draw reported nothing.
void theHomeValueCardOffersAResolver() {
  assert(findCard(HomeValue::kCardId) != nullptr && "HomeValue registered no card");
  assert(hasResolver(HomeValue::kCardId) &&
         "the home-value card registers no listingId, so every draw reports no content key");
  std::cout << "  the home-value card registers a content-key resolver\n";
}

/// A draw with an estimate and an address reports that address, exactly as the
/// server sent it.
void aHomeValueDrawReportsItsAddress() {
  setHomeValue(485000, "104 Virginia Ave, Edgewater, MD 21037");
  assert(keyFor(HomeValue::kCardId, 0) == "104 Virginia Ave, Edgewater, MD 21037");

  setHomeValue(949000, "300 Eatons Landing Dr, Annapolis, MD 21401");
  assert(keyFor(HomeValue::kCardId, 0) == "300 Eatons Landing Dr, Annapolis, MD 21401");
  std::cout << "  a home-value draw reports the address the estimate was computed for\n";
}

/// The key is the property and not the figure.
///
/// This is the whole gain: production holds two estimates for each of two
/// addresses, and grouping two quarter-hours of the same house together is what a
/// free-text label cannot do.
void theKeyIsTheAddressAndNotTheEstimate() {
  setHomeValue(485000, "104 Virginia Ave, Edgewater, MD 21037");
  const std::string first = keyFor(HomeValue::kCardId, 0);

  setHomeValue(526000, "104 Virginia Ave, Edgewater, MD 21037");
  const std::string second = keyFor(HomeValue::kCardId, 0);

  assert(first == second && "the same house under a new estimate must not start a new row");
  std::cout << "  an estimate that moved reports the same key for the same house\n";
}

/// No estimate, no key.
///
/// cardItemCount() reports zero without one and the card leaves the rotation, so
/// a draw here puts "--" where the figure goes and names no property.
void aDrawWithNoEstimateReportsNothing() {
  setHomeValue(-1, "104 Virginia Ave, Edgewater, MD 21037");
  assert(keyFor(HomeValue::kCardId, 0).empty() &&
         "a card with no estimate drew no valuation, so it names no property");
  std::cout << "  a draw with no estimate reports no key\n";
}

/// An estimate resolved from a position fix reports nothing.
///
/// RentCast returns no formatted address for a lat/lon query, the card draws the
/// figure with no headline, and there is then nothing that says which property it
/// was about.
void aValuationFromAPositionFixReportsNothing() {
  setHomeValue(615000, "");
  assert(keyFor(HomeValue::kCardId, 0).empty() &&
         "a valuation with no address identifies no property");
  std::cout << "  a valuation resolved from a position fix reports no key\n";
}

/// An address that changed reports the new one at once.
void anAddressThatChangedReportsTheNewOne() {
  setHomeValue(485000, "104 Virginia Ave, Edgewater, MD 21037");
  setHomeValue(615000, "PO Box 4202, Annapolis, MD 21114");
  assert(keyFor(HomeValue::kCardId, 0) == "PO Box 4202, Annapolis, MD 21114");
  std::cout << "  an address that changed reports the new one\n";
}

/// An item past the card's one reports nothing.
void anItemPastTheHomeValueCardReportsNothing() {
  setHomeValue(485000, "104 Virginia Ave, Edgewater, MD 21037");
  assert(!keyFor(HomeValue::kCardId, 0).empty());
  assert(keyFor(HomeValue::kCardId, 1).empty());
  assert(keyFor(HomeValue::kCardId, 97).empty());
  std::cout << "  an item past the home-value card's one reports nothing\n";
}

// --- announcements ----------------------------------------------------------

/// All five instances declare a resolver.
void everyNoticeCardOffersAResolver() {
  for (const char* const id : kNoticeIds) {
    assert(findCard(id) != nullptr && "an announcement instance registered no card");
    assert(hasResolver(id) &&
           "an announcement instance registers no listingId, so its draws report no key");
  }
  std::cout << "  all five announcement cards register a content-key resolver\n";
}

/// A configured notice reports sixteen lowercase hex characters.
void aNoticeReportsADigestOfItsText() {
  setNoticeText("announcement", "Trash goes out tonight");
  const std::string key = keyFor("announcement", 0);

  assert(key.length() == 16 && "the digest is sixteen hex characters");
  for (const char c : key) {
    assert(((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) && "lowercase hex only");
  }
  std::cout << "  a configured notice reports a sixteen-character digest\n";
}

/// The digest is FNV-1a 64, and this is the vector that says so.
///
/// Computed outside this tree rather than by this algorithm: the published FNV-1a
/// 64 vectors for "", "a" and "abc" are cbf29ce484222325, af63dc4c8601ec8c and
/// e71fa2190541574b, and the implementation that reproduces all three gives
/// abacb6c3112e90e4 for the text below. Without this the digest could change
/// shape in a later edit and every other case here would still pass, while every
/// period row written before the change stopped matching the ones after it.
void theDigestIsTheOneThisKeyPromises() {
  setNoticeText("announcement", "Trash goes out tonight");
  assert(keyFor("announcement", 0) == "abacb6c3112e90e4");
  std::cout << "  the digest matches the FNV-1a 64 vector for a known notice\n";
}

/// An accented notice digests its bytes unsigned.
///
/// A Spanish or French notice carries bytes above 0x7F, `char` is signed on this
/// target, and without the cast in textDigest() those bytes sign-extend into the
/// hash. Every ASCII case above passes either way, so this is the one that pins
/// it. The text is written as explicit bytes rather than as source in another
/// encoding, so what is hashed does not depend on how this file was saved.
///
/// Computed outside this tree the same way the ASCII vector above was: the bytes
/// are 50 69 c3 b1 61 74 61 20 61 20 6c 61 73 20 73 65 69 73, which is "Pinata a
/// las seis" with the n-tilde in UTF-8, and an implementation reading each byte
/// unsigned gives the value below. Reading them as signed char gives
/// 705e2c0f2b64b19f instead, which is what this case fails with.
void anAccentedNoticeDigestsItsBytesUnsigned() {
  const std::string accented = std::string("Pi") + "\xc3\xb1" + "ata a las seis";
  assert(accented.length() == 18 && "the n-tilde is two bytes");

  setNoticeText("announcement", accented);
  assert(keyFor("announcement", 0) == "0db4397fe9544d9f");
  std::cout << "  an accented notice digests its bytes unsigned\n";
}

/// Two notices that differ report different keys.
void twoDifferentNoticesReportDifferentKeys() {
  setNoticeText("announcement", "Trash goes out tonight");
  const std::string first = keyFor("announcement", 0);

  setNoticeText("announcement", "Recycling goes out tonight");
  const std::string second = keyFor("announcement", 0);

  assert(first != second);
  std::cout << "  two different notices report different keys\n";
}

/// Two long notices sharing their first 64 characters report different keys.
///
/// This is why the key is a digest rather than the text. A content key is bounded
/// at 64 characters on the device and again in the rollup, announcement text runs
/// to 280, and every announcement drawing in production on 2026-11-03 was already
/// 74 to 77 characters - so sending the text itself would have been truncated on
/// the ordinary draw, not the exceptional one, and these two would have grouped as
/// one row.
void twoNoticesSharingTheirFirstSixtyFourCharactersDiffer() {
  const std::string shared(64, 'x');
  assert(shared.length() == 64);

  setNoticeText("announcement", shared + "ends Tuesday");
  const std::string tuesday = keyFor("announcement", 0);

  setNoticeText("announcement", shared + "ends Wednesday");
  const std::string wednesday = keyFor("announcement", 0);

  assert(tuesday != wednesday &&
         "two notices differing past the 64th character must not report one key");
  std::cout << "  two notices sharing their first 64 characters report different keys\n";
}

/// The same notice on two instances reports the same key.
///
/// The period row is unique on device, card id, content key and period, so the
/// card id is what tells slot two from slot three. The key answers what was on the
/// glass, and the same words are the same content.
void theSameNoticeOnTwoInstancesReportsTheSameKey() {
  setNoticeText("announcement2", "Pool closes at eight");
  setNoticeText("announcement3", "Pool closes at eight");
  assert(keyFor("announcement2", 0) == keyFor("announcement3", 0));
  std::cout << "  the same notice on two instances reports the same key\n";
}

/// Two instances holding different notices report different keys.
///
/// Each instance reads its own descriptor, so this is what says the resolver is
/// asking the right one rather than reaching for slot one's text.
void twoInstancesWithDifferentNoticesReportDifferentKeys() {
  setNoticeText("announcement2", "Pool closes at eight");
  setNoticeText("announcement3", "Gate code changes Monday");
  assert(keyFor("announcement2", 0) != keyFor("announcement3", 0));
  std::cout << "  two instances holding different notices report different keys\n";
}

/// A notice nobody wrote reports nothing.
///
/// An unconfigured instance reports zero items and never appears; the one draw
/// that can still reach it is a policy changing between the scheduler's
/// itemCount() check and the call, which draws the no-content state.
void anUnconfiguredNoticeReportsNothing() {
  setNoticeText("announcement4", "");
  assert(keyFor("announcement4", 0).empty() &&
         "a card that drew the no-content state names no notice");
  std::cout << "  an unconfigured notice reports no key\n";
}

/// Text that changed under the card reports the new key with no fetch.
///
/// itemCount() and draw() both re-read the descriptor on every call rather than
/// caching, and the resolver reads it the same way, so the key names the words
/// that are configured now.
void aNoticeThatChangedReportsTheNewKey() {
  setNoticeText("announcement5", "Street sweeping Thursday");
  const std::string before = keyFor("announcement5", 0);

  setNoticeText("announcement5", "Street sweeping Friday");
  assert(keyFor("announcement5", 0) != before);
  std::cout << "  a notice that changed reports the new key at once\n";
}

/// An item past the instance's one reports nothing.
void anItemPastANoticeReportsNothing() {
  setNoticeText("announcement", "Trash goes out tonight");
  assert(!keyFor("announcement", 0).empty());
  assert(keyFor("announcement", 1).empty());
  assert(keyFor("announcement", 97).empty());
  std::cout << "  an item past a notice's one reports nothing\n";
}

}  // namespace

// --- the production files' dependencies -------------------------------------

namespace Cards {

bool registerCard(const CardSpec& spec) {
  if (registry().size() >= registry().capacity()) {
    return false;
  }
  registry().push_back(spec);
  return true;
}

uint8_t count() { return static_cast<uint8_t>(registry().size()); }

CardSpec& at(uint8_t index) { return registry().at(index); }

int8_t indexOf(const char* id) {
  for (size_t i = 0; i < registry().size(); ++i) {
    if (std::strcmp(registry()[i].id, id) == 0) {
      return static_cast<int8_t>(i);
    }
  }
  return -1;
}

}  // namespace Cards

namespace Display {

int utcOffsetMinutes() { return -300; }

void showHomeValueCard(const String& address, const String& estimateText,
                       const String& rangeText, const String& detailText) {
  (void)address;
  (void)estimateText;
  (void)rangeText;
  (void)detailText;
}

void showAnnouncementCard(const String& text) { (void)text; }

void showNoContent(const String& headline, const String& detail) {
  (void)headline;
  (void)detail;
}

}  // namespace Display

namespace Log {

void printf(const char* format, ...) {
  (void)format;
}

void verbose(const char* format, ...) {
  (void)format;
}

void line(const String& text) { (void)text; }

void line(const char* text) { (void)text; }

}  // namespace Log

int main() {
  theHomeValueCardOffersAResolver();
  aHomeValueDrawReportsItsAddress();
  theKeyIsTheAddressAndNotTheEstimate();
  aDrawWithNoEstimateReportsNothing();
  aValuationFromAPositionFixReportsNothing();
  anAddressThatChangedReportsTheNewOne();
  anItemPastTheHomeValueCardReportsNothing();

  everyNoticeCardOffersAResolver();
  aNoticeReportsADigestOfItsText();
  theDigestIsTheOneThisKeyPromises();
  anAccentedNoticeDigestsItsBytesUnsigned();
  twoDifferentNoticesReportDifferentKeys();
  twoNoticesSharingTheirFirstSixtyFourCharactersDiffer();
  theSameNoticeOnTwoInstancesReportsTheSameKey();
  twoInstancesWithDifferentNoticesReportDifferentKeys();
  anUnconfiguredNoticeReportsNothing();
  aNoticeThatChangedReportsTheNewKey();
  anItemPastANoticeReportsNothing();

  std::cout << "PASS: announcement and home value content keys (18 cases)\n";
  return 0;
}
