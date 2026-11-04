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
#include <cstdarg>
#include <cstdio>
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

/// Every line the module logged since the last clearLog(), formatted the way
/// Log::printf formats it. A draw that put no picture up is only readable on the
/// remote stream, so what it writes there is as much the subject of this file as
/// the content key is.
std::vector<std::string> gLogLines;

void clearLog() { gLogLines.clear(); }

/// Lines holding every one of `fragments`.
std::vector<std::string> loggedMatching(const std::vector<std::string>& fragments) {
  std::vector<std::string> found;
  for (const auto& line : gLogLines) {
    bool all = true;
    for (const auto& fragment : fragments) {
      if (line.find(fragment) == std::string::npos) {
        all = false;
        break;
      }
    }
    if (all) {
      found.push_back(line);
    }
  }
  return found;
}

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

/// A draw that put no photograph up says so, and names which item held the glass
/// instead.
///
/// This is the half of the card that a reader cannot get at any other way. A
/// cardless display holds one picture, the other items of the set draw
/// "No picture to show", and the impression recorded against each of them carries
/// the full dwell - so the count on the card reads higher than the pictures it
/// put up and nothing on the stream distinguished the two. The draws that fail to
/// decode already wrote a line; these did not.
void aDrawWithNoPictureNamesWhatTookTheGlass() {
  gOnCard.clear();
  gRamFetchSucceeds = true;
  gDrawCachedSucceeds = true;
  resetTo({kFirst, kSecond, kThird});
  runFetch();

  // Whichever item the heap buffer landed on is the one that drew. The resolver
  // answers for exactly that item, which is what ties this case to the key.
  int held = -1;
  for (uint16_t item = 0; item < 3; ++item) {
    if (!keyFor(item).empty()) {
      held = static_cast<int>(item);
    }
  }
  assert(held >= 0 && "a cardless display should hold one picture after a fetch");

  assert(registered().draw != nullptr);
  const char* const ids[3] = {kFirst, kSecond, kThird};

  for (uint16_t item = 0; item < 3; ++item) {
    clearLog();
    registered().draw(item);

    const auto named = loggedMatching({"personalphotos", ids[item], "not drawn"});
    if (static_cast<int>(item) == held) {
      assert(named.empty() && "the photograph that drew must not report itself as not drawn");
      continue;
    }

    assert(named.size() == 1 &&
           "a draw that put no photograph up writes one line naming the photograph");
    const std::string& line = named.front();
    assert(line.find("item " + std::to_string(item + 1) + " of 3") != std::string::npos &&
           "the line names which item of the set drew nothing");

    // The held item by number, not the word "heap". Both branches of production's
    // line carry "heap", so an assertion on that word alone passes whichever one
    // ran and says nothing about which state the display was in.
    assert(line.find("item " + std::to_string(held + 1) + " is the one held in heap") !=
               std::string::npos &&
           "the line names which item holds the heap buffer instead");
    assert(line.find("nothing is held in heap") == std::string::npos &&
           "a display holding a picture must not report that nothing is held");
  }

  std::cout << "  a draw with no photograph names the item and what holds the glass\n";
}

/// A cardless display holding nothing says so, rather than naming a held item.
///
/// The other arm of the same line. A fetch that failed leaves the heap buffer
/// empty, so every item of the set draws the no-content state and there is no
/// held item to point at. Forcing production's branch either way satisfied an
/// assertion on the word "heap", which both arms carry.
void aDrawWithNothingHeldSaysNothingIsHeld() {
  gOnCard.clear();
  gRamFetchSucceeds = false;
  gDrawCachedSucceeds = true;
  resetTo({kFirst, kSecond});
  runFetch();

  for (uint16_t item = 0; item < 2; ++item) {
    assert(keyFor(item).empty() && "a fetch that failed holds nothing to report");
  }

  const char* const ids[2] = {kFirst, kSecond};
  for (uint16_t item = 0; item < 2; ++item) {
    clearLog();
    registered().draw(item);

    const auto named = loggedMatching({"personalphotos", ids[item], "not drawn"});
    assert(named.size() == 1 &&
           "a draw with nothing held writes one line naming the photograph");
    const std::string& line = named.front();
    assert(line.find("item " + std::to_string(item + 1) + " of 2") != std::string::npos &&
           "the line names which item of the set drew nothing");
    assert(line.find("nothing is held in heap") != std::string::npos &&
           "the line says the heap buffer holds nothing");
    assert(line.find("is the one held in heap") == std::string::npos &&
           "a display holding nothing must not name a held item");
  }

  gRamFetchSucceeds = true;
  std::cout << "  a draw with nothing held says nothing is held, rather than naming an item\n";
}

/// An item past the end of the set says so too.
///
/// CardManager asks for items under itemCount(), so this is the path that means
/// the two disagree. It drew the no-content state and wrote nothing.
void anItemPastTheSetSaysItDrewNothing() {
  gOnCard = {kFirst};
  gRamFetchSucceeds = true;
  resetTo({kFirst});
  runFetch();

  clearLog();
  assert(registered().draw != nullptr);
  registered().draw(4);

  const auto named = loggedMatching({"personalphotos", "item 5", "set holds 1"});
  assert(named.size() == 1 &&
         "a draw past the end of the set writes one line naming the item and the set size");
  std::cout << "  an item past the set reports the item and the set size\n";
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

bool registerCard(const CardSpec& spec, KeyPolicy keyPolicy) {
  // The check production's registerCard() applies, reached through the same
  // predicate, so a module compiled here that states a policy its descriptor
  // does not satisfy stops this test rather than a device. PersonalPhotos.cpp
  // registers as Resolves and assigns a listingId.
  assert(keyPolicyHolds(spec, keyPolicy));
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

/// The same 256-byte scratch and the same vsnprintf production formats into, so
/// a line this test reads back is the line that reaches the stream.
void printf(const char* format, ...) {
  char scratch[256];
  va_list args;
  va_start(args, format);
  vsnprintf(scratch, sizeof(scratch), format, args);
  va_end(args);
  gLogLines.push_back(scratch);
}

}  // namespace Log

int main() {
  theCardOffersAResolver();
  aCachedPhotographReportsItsId();
  aCardlessDisplayReportsOnlyThePictureItHolds();
  aFetchThatFailedReportsNothing();
  anItemPastTheSetReportsNothing();
  aChangedSetReportsNothingUntilTheNextFetch();
  aPhotographThatDidNotDecodeStopsReportingItsId();
  aDrawWithNoPictureNamesWhatTookTheGlass();
  aDrawWithNothingHeldSaysNothingIsHeld();
  anItemPastTheSetSaysItDrewNothing();

  std::cout << "PASS: personal photos content key (10 cases)\n";
  return 0;
}
