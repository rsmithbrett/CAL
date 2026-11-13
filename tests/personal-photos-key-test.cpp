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

/// The id of the last photograph a draw actually put on the panel, or empty.
/// What this card draws is a photograph or nothing, so "did a picture reach the
/// glass" is a question every case about the drawable set has to be able to ask.
std::string gLastDrawn;

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

/// A photograph that failed to decode leaves the drawable set.
///
/// Production drops the item until the next fetch, and the key follows that
/// decision rather than holding its own copy of what is drawable. The index
/// space is the drawable photographs, so the set compacts and the dropped id is
/// no longer reported by any index.
void aPhotographThatDidNotDecodeLeavesTheSet() {
  gOnCard = {kFirst, kSecond};
  gRamFetchSucceeds = true;
  gDrawCachedSucceeds = true;
  resetTo({kFirst, kSecond});
  runFetch();
  assert(registered().itemCount() == 2);
  assert(keyFor(0) == kFirst);
  assert(keyFor(1) == kSecond);

  gDrawCachedSucceeds = false;
  assert(registered().draw != nullptr);
  registered().draw(0);

  assert(registered().itemCount() == 1 &&
         "an item dropped for a decode failure is no longer an item");
  assert(keyFor(0) == kSecond &&
         "the drawable set compacts onto the photographs that are left");
  assert(keyFor(1).empty());
  for (uint16_t item = 0; item < PersonalPhotos::kMaxPhotos; ++item) {
    assert(keyFor(item) != std::string(kFirst) &&
           "an item dropped for a decode failure must not keep reporting its id");
  }
  gDrawCachedSucceeds = true;
  std::cout << "  a photograph that did not decode leaves the drawable set\n";
}

/// A cardless display counts the one photograph it holds, not the whole set.
///
/// This is the state the photograph that started this came from: three chosen,
/// one in heap, and the other two taking a turn each with "No picture to show"
/// on the panel. They are not items any more, so the card takes one turn and a
/// photograph is on it.
void aCardlessDisplayCountsOnlyTheOneItHolds() {
  gOnCard.clear();
  gRamFetchSucceeds = true;
  gDrawCachedSucceeds = true;
  resetTo({kFirst, kSecond, kThird});
  runFetch();

  assert(registered().itemCount() == 1 &&
         "a display with no SD card can draw one of the set, so it counts one");

  clearLog();
  gLastDrawn.clear();
  assert(registered().draw != nullptr);
  registered().draw(0);

  assert(!gLastDrawn.empty() && "the one item it counts draws a photograph");
  assert(gLastDrawn == keyFor(0) && "and it is the photograph the key names");
  assert(loggedMatching({"personalphotos", "nothing drawn"}).empty() &&
         "a turn that drew a photograph writes no nothing-drawn line");

  std::cout << "  a cardless display counts the one photograph it holds\n";
}

/// A cardless display whose fetch failed counts nothing, so it takes no turn.
///
/// Every item of the set used to draw the no-content state here. None of them
/// is an item now, the card is passed over the way the aircraft card is passed
/// over without a plane, and a draw forced anyway puts no picture up.
void aFetchThatHeldNothingCountsZero() {
  gOnCard.clear();
  gRamFetchSucceeds = false;
  gDrawCachedSucceeds = true;
  resetTo({kFirst, kSecond});
  runFetch();

  assert(registered().itemCount() == 0 &&
         "nothing on the card and nothing in heap is nothing to draw");
  for (uint16_t item = 0; item < 2; ++item) {
    assert(keyFor(item).empty() && "a fetch that failed holds nothing to report");
  }

  clearLog();
  gLastDrawn.clear();
  registered().draw(0);
  assert(gLastDrawn.empty() && "asked to draw anyway, it puts no photograph up");
  assert(loggedMatching({"personalphotos", "nothing drawn"}).size() == 1 &&
         "and writes one line saying so instead of putting a screen up");

  gRamFetchSucceeds = true;
  std::cout << "  a fetch that held nothing counts zero and draws nothing\n";
}

/// An index no photograph answers draws nothing and names what it was asked.
///
/// CardManager asks for items under itemCount(), so this is the path that means
/// the two disagree. It draws no picture and writes the one line that says so.
void anIndexNoPhotographAnswersDrawsNothing() {
  gOnCard = {kFirst};
  gRamFetchSucceeds = true;
  gDrawCachedSucceeds = true;
  resetTo({kFirst});
  runFetch();
  assert(registered().itemCount() == 1);

  clearLog();
  gLastDrawn.clear();
  assert(registered().draw != nullptr);
  registered().draw(4);

  assert(gLastDrawn.empty() && "an index past the drawable set puts no photograph up");
  const auto named = loggedMatching({"personalphotos", "item 5", "1 drawable", "nothing drawn"});
  assert(named.size() == 1 &&
         "it writes one line naming the index asked for and how many are drawable");
  std::cout << "  an index no photograph answers draws nothing\n";
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
  if (!gDrawCachedSucceeds) {
    return false;
  }
  gLastDrawn = static_cast<const std::string&>(id);
  return true;
}

bool drawRam(const String& id, const RamAssetBuffer& buffer) {
  if (buffer.data == nullptr) {
    return false;
  }
  gLastDrawn = static_cast<const std::string&>(id);
  return true;
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

// No Display namespace. This card draws a photograph or nothing, so there is no
// Display function left for it to call - and leaving showNoContent() unstubbed
// means a draw path that reaches for a screen again fails to link here rather
// than reaching a panel.

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
  aPhotographThatDidNotDecodeLeavesTheSet();
  aCardlessDisplayCountsOnlyTheOneItHolds();
  aFetchThatHeldNothingCountsZero();
  anIndexNoPhotographAnswersDrawsNothing();

  std::cout << "PASS: personal photos content key (10 cases)\n";
  return 0;
}
