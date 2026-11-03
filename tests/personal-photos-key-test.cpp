// App/PersonalPhotos.cpp: the content key a draw of the household's
// photographs reports, and the two conditions that decide whether it reports
// one at all.
//
// The production file is compiled as it ships. Assets, Cards, Display and Log
// are declarations that parse on a host once Arduino.h is stubbed, and the
// functions PersonalPhotos.cpp calls into are defined below. The Cards stub
// keeps the descriptor the module registers, which is the only way to reach its
// resolver: cardListingId() sits in an anonymous namespace and
// CardManager::noteShowing() reaches it the same way, through
// CardSpec::listingId.
//
// What the cases are here for is CARD_IMPRESSIONS_DESIGN.md, "Which attribute
// says what the draw was about", and PERSONAL_PHOTOS_DESIGN.md section 8.7. The
// server keys a quarter-hour period row on the content key, so a card reporting
// none collapses every item it draws into one row and a report cannot say which
// photograph was up.
//
// The gate on drawability is the half that is not about reporting: a display
// with no SD card holds one picture in heap, the rest of the set draws the
// no-content state, and an id on those draws would name a photograph that was
// not on the glass.

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "Assets.h"
#include "Cards.h"
#include "Display.h"
#include "Log.h"
#include "PersonalPhotos.h"

namespace {

// --- what the stubs are told to do ------------------------------------------

/// Ids ensureCached() answers true for, which is what an SD-backed display
/// holds.
std::vector<std::string> gOnCard;

/// Whether fetchToRam() succeeds. A cardless display whose fetch fails holds
/// nothing at all, which is its own case below.
bool gRamFetchSucceeds = true;

/// Whether drawCached() succeeds. False is the decode failure production drops
/// an item for.
bool gDrawCachedSucceeds = true;

/// Bytes the RAM stub hands out, so the module's release path has something to
/// clear.
uint8_t gRamBytes[16] = {};

/// The descriptor PersonalPhotos registered, in a function-local static so
/// nothing depends on the order two translation units initialise in. The module
/// registers at static-init time, before main() runs.
Cards::CardSpec& registered() {
  static Cards::CardSpec spec;
  return spec;
}

bool& wasRegistered() {
  static bool yes = false;
  return yes;
}

bool heldOnCard(const String& id) {
  for (const auto& held : gOnCard) {
    if (held == static_cast<const std::string&>(id)) {
      return true;
    }
  }
  return false;
}

// --- helpers ----------------------------------------------------------------

/// Three canonical ids, the shape the check-in response carries.
const char* const kFirst = "6d0c3a2d-4e25-4356-b6df-e57bd5ac0b4a";
const char* const kSecond = "e409c676-3926-48aa-9e6c-719e9a1b5188";
const char* const kThird = "1edb7bf9-904c-43e2-989c-328436c6e45d";

/// Hands the module a set, the way a check-in does.
void applySet(const std::vector<const char*>& ids) {
  char buffer[PersonalPhotos::kMaxPhotos][PersonalPhotos::kAssetIdLength + 1] = {};
  for (size_t i = 0; i < ids.size(); ++i) {
    std::strncpy(buffer[i], ids[i], PersonalPhotos::kAssetIdLength);
  }
  PersonalPhotos::setFromCheckIn(buffer, static_cast<uint8_t>(ids.size()));
}

/// An id no case uses, for the placeholder below.
const char* const kPlaceholder = "00000000-0000-4000-8000-000000000000";

/// Hands the module a set with nothing held for it yet.
///
/// A set equal to the one already held returns early in production, which keeps
/// what fetch() recorded about the previous case. The placeholder set ahead of
/// the wanted one is what makes both calls a change, and a change is what clears
/// the held state. Every case that wants a known starting point goes through
/// here; the one case about a set changing calls applySet() directly, because
/// the change is what it is measuring.
void resetTo(const std::vector<const char*>& ids) {
  applySet({kPlaceholder});
  applySet(ids);
}

/// The key the registered resolver answers for one item.
std::string keyFor(uint16_t item) {
  assert(registered().listingId != nullptr);
  return registered().listingId(item);
}

void runFetch() {
  assert(registered().fetch != nullptr);
  registered().fetch();
}

// --- cases ------------------------------------------------------------------

/// The card declares a resolver at all.
///
/// This is the case the card shipped failing. Its ids ride the check-in
/// response rather than the card policy, so the assetId and qrData
/// CardManager::noteShowing falls back to are both empty and no resolver means
/// every draw reports nothing.
void theCardOffersAResolver() {
  assert(wasRegistered() && "PersonalPhotos registered no card");
  assert(registered().listingId != nullptr &&
         "the photographs card registers no listingId, so every draw reports no content key");
  std::cout << "  the card registers a content-key resolver\n";
}

/// An SD-backed display reports the id of whichever photograph is up.
void aCachedPhotographReportsItsId() {
  gOnCard = {kFirst, kSecond, kThird};
  gRamFetchSucceeds = true;
  resetTo({kFirst, kSecond, kThird});
  runFetch();

  assert(keyFor(0) == kFirst);
  assert(keyFor(1) == kSecond);
  assert(keyFor(2) == kThird);
  std::cout << "  a photograph on the card reports its own asset id\n";
}

/// A cardless display reports the one it holds and nothing for the rest.
///
/// The set still cycles, because fetch() moves the heap buffer along, so this
/// also pins that the key follows the buffer rather than staying on item zero.
void aCardlessDisplayReportsOnlyThePictureItHolds() {
  gOnCard.clear();
  gRamFetchSucceeds = true;
  resetTo({kFirst, kSecond, kThird});

  for (uint16_t item = 0; item < 3; ++item) {
    assert(keyFor(item).empty() && "nothing is held before the first fetch");
  }

  std::vector<std::string> heldInOrder;
  for (int pass = 0; pass < 3; ++pass) {
    runFetch();

    std::string held;
    int keysThisPass = 0;
    for (uint16_t item = 0; item < 3; ++item) {
      const std::string key = keyFor(item);
      if (!key.empty()) {
        ++keysThisPass;
        held = key;
      }
    }
    assert(keysThisPass == 1 &&
           "a display with no SD card holds one picture, so one item reports a key");
    heldInOrder.push_back(held);
  }

  assert(heldInOrder[0] == kFirst);
  assert(heldInOrder[1] == kSecond);
  assert(heldInOrder[2] == kThird);
  std::cout << "  a cardless display reports the picture in heap and nothing for the others\n";
}

/// A cardless display whose fetch failed reports nothing for any item.
void aFetchThatFailedReportsNothing() {
  gOnCard.clear();
  gRamFetchSucceeds = false;
  resetTo({kFirst, kSecond});
  runFetch();

  assert(keyFor(0).empty());
  assert(keyFor(1).empty());
  std::cout << "  a fetch that failed reports no key rather than an id nothing drew\n";
}

/// An item past the end of the set reports nothing.
void anItemPastTheSetReportsNothing() {
  gOnCard = {kFirst};
  gRamFetchSucceeds = true;
  resetTo({kFirst});
  runFetch();

  assert(keyFor(0) == kFirst);
  assert(keyFor(1).empty());
  assert(keyFor(PersonalPhotos::kMaxPhotos).empty());
  std::cout << "  an item past the set reports nothing\n";
}

/// A set that changed reports nothing until the next fetch.
///
/// setFromCheckIn() clears what it knows is on the card and hands the heap
/// buffer back, so the window between a household editing their choice and the
/// next refresh reports no key rather than the previous set's.
void aChangedSetReportsNothingUntilTheNextFetch() {
  gOnCard = {kFirst, kSecond};
  gRamFetchSucceeds = true;
  resetTo({kFirst, kSecond});
  runFetch();
  assert(keyFor(0) == kFirst);

  // The change itself, with no placeholder in between: this is the one case
  // measuring what a household editing their choice does to the key.
  gOnCard = {kThird};
  applySet({kThird});
  assert(keyFor(0).empty() && "the set moved and nothing has been fetched for it yet");

  runFetch();
  assert(keyFor(0) == kThird);
  std::cout << "  a set that changed reports nothing until the next fetch\n";
}

/// A photograph that failed to decode stops reporting its id.
///
/// Production drops the item until the next fetch, and the key follows that
/// decision rather than holding its own copy of what is drawable.
void aPhotographThatDidNotDecodeStopsReportingItsId() {
  gOnCard = {kFirst, kSecond};
  gRamFetchSucceeds = true;
  gDrawCachedSucceeds = true;
  resetTo({kFirst, kSecond});
  runFetch();
  assert(keyFor(0) == kFirst);

  gDrawCachedSucceeds = false;
  assert(registered().draw != nullptr);
  registered().draw(0);

  assert(keyFor(0).empty() &&
         "an item dropped for a decode failure must not keep reporting its id");
  assert(keyFor(1) == kSecond);
  gDrawCachedSucceeds = true;
  std::cout << "  a photograph that did not decode stops reporting its id\n";
}

}  // namespace

// --- the production file's dependencies -------------------------------------

namespace Assets {

bool ensureCached(const String& id) { return heldOnCard(id); }

bool fetchToRam(const String& id, RamAssetBuffer& buffer) {
  (void)id;
  // Committed before the call can fail, which is the state production's
  // releaseRam() remarks describe.
  buffer.data = gRamBytes;
  buffer.capacity = sizeof(gRamBytes);
  buffer.size = gRamFetchSucceeds ? sizeof(gRamBytes) : 0;
  return gRamFetchSucceeds;
}

void releaseRamBuffer(RamAssetBuffer& buffer) {
  buffer.data = nullptr;
  buffer.capacity = 0;
  buffer.size = 0;
}

bool drawCached(const String& id) {
  (void)id;
  return gDrawCachedSucceeds;
}

bool drawRam(const String& id, const RamAssetBuffer& buffer) {
  (void)id;
  return buffer.data != nullptr;
}

void invalidate(const String& id) { (void)id; }

}  // namespace Assets

namespace Cards {

bool registerCard(const CardSpec& spec) {
  registered() = spec;
  wasRegistered() = true;
  return true;
}

}  // namespace Cards

namespace Display {

void showNoContent(const String& headline, const String& detail) {
  (void)headline;
  (void)detail;
}

}  // namespace Display

namespace Log {

void printf(const char* format, ...) { (void)format; }

}  // namespace Log

int main() {
  theCardOffersAResolver();
  aCachedPhotographReportsItsId();
  aCardlessDisplayReportsOnlyThePictureItHolds();
  aFetchThatFailedReportsNothing();
  anItemPastTheSetReportsNothing();
  aChangedSetReportsNothingUntilTheNextFetch();
  aPhotographThatDidNotDecodeStopsReportingItsId();

  std::cout << "PASS: personal photos content key (7 cases)\n";
  return 0;
}
