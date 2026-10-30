#include "PersonalPhotos.h"

#include <cstring>

#include "Assets.h"
#include "Cards.h"
#include "Display.h"
#include "Log.h"

namespace PersonalPhotos {

namespace {

const char* const kCardId = "personalphotos";

// A truncated id is a well-formed id for some other asset, so the two limits
// must not drift - CardManager::applyPolicy drops an over-long assetId for
// exactly that reason, and Graphic.cpp has the same assert. Catching it here
// makes a divergence a compile error rather than a household's display drawing
// somebody else's picture.
static_assert(kAssetIdLength >= 36,
              "An asset id is a canonical GUID; 36 characters is the floor.");

char gIds[kMaxPhotos][kAssetIdLength + 1] = {};
uint8_t gCount = 0;

/// Which ids are on the SD card, filled by fetch() and read by draw(). An item
/// that is not ready draws its own no-content state rather than reaching for
/// the network, which is what Cards.h's fetch/draw split is for.
bool gOnCard[kMaxPhotos] = {};

/// The one picture held in heap rather than on SD, for a display with no card.
///
/// One buffer, not ten. Only one photograph is on screen at a time and ten at
/// ten to twenty kilobytes apiece is several times what this device has spare.
/// fetch() moves it to the next item each time it runs, so a cardless display
/// cycles through the set at the refresh cadence rather than the dwell one.
Assets::RamAssetBuffer gRamBuffer;

/// Which item gRamBuffer holds, or kNothingHeld.
constexpr uint8_t kNothingHeld = 0xFF;
uint8_t gRamItem = kNothingHeld;

/// Which item fetch() will try to hold next on a cardless display.
uint8_t gRamNext = 0;

uint8_t gLastLoggedCount = 255;

/// How many removed ids the set-change line names before it falls back to a
/// count. The cap Assets::decodeFailureIds uses, for the same reason: Log's
/// scratch buffer is 256 bytes and ten canonical GUIDs do not fit in one line.
constexpr uint8_t kLoggedRemovals = 4;

/// Whether `id` is one of the first `count` entries of `ids`.
///
/// A linear scan, run at most kMaxPhotos times and only on a set change. A
/// hundred strcmps of 36 bytes once a family edits their choice is cheaper than
/// anything that would need building first.
bool isInSet(const char* id, const char ids[][kAssetIdLength + 1], uint8_t count) {
  for (uint8_t i = 0; i < count; ++i) {
    if (std::strcmp(id, ids[i]) == 0) {
      return true;
    }
  }
  return false;
}

/// Deletes the cached file for every photograph that was in the held set and is
/// not in `ids`.
///
/// A display outlives a household. When a device is reassigned, the next
/// check-in carries another family's ids, the previous family's pictures stop
/// being drawn, and their bytes stay on the card for anyone who takes it out.
/// PERSONAL_PHOTOS_DESIGN.md section 5.1 scopes a household's pictures to the
/// household that owns them; section 15.2 destroys a library when its last
/// display goes away, which makes this card the last place those bytes exist.
///
/// Reads gIds, so it runs before the new ids are copied over them -
/// setFromCheckIn writes that array in place.
///
/// An id still in the new set is left alone. The family wants it drawn, and
/// re-fetching a picture already on the card spends the round trip this cache
/// exists to avoid.
///
/// Assets::invalidate returns without doing anything when Sd::isReady() is
/// false, so on a cardless display this loop runs and removes nothing.
void invalidateDeparted(const char ids[][kAssetIdLength + 1], uint8_t count) {
  uint8_t removed = 0;
  String named;

  for (uint8_t i = 0; i < gCount; ++i) {
    if (isInSet(gIds[i], ids, count)) {
      continue;
    }

    Assets::invalidate(String(gIds[i]));
    ++removed;

    if (removed <= kLoggedRemovals) {
      if (named.length() > 0) {
        named += ", ";
      }
      named += gIds[i];
    }
  }

  if (removed == 0) {
    return;
  }

  if (removed > kLoggedRemovals) {
    named += " and ";
    named += static_cast<unsigned>(removed - kLoggedRemovals);
    named += " more";
  }

  // One line per set change rather than one per id. The remote debug stream is
  // a deployed display's only diagnostic channel, so a deletion nobody can read
  // back is a deletion nobody can confirm happened.
  Log::printf("[%s] dropped %u cached photograph(s) no longer in the set: %s", kCardId,
              static_cast<unsigned>(removed), named.c_str());
}

/// Hands the buffer back.
///
/// GATED ON THE POINTER, NOT ON WHAT IT HOLDS. fetchToRam commits buffer.data
/// before it can fail, so a fetch that times out or whose hash mismatches
/// leaves bytes held with no item recorded against them. Gating on the item
/// would early-return forever from that state: the check-in failure path could
/// not reclaim it, setFromCheckIn could not, and a later SD success could not -
/// and a device whose fetches fail is the device whose check-ins fail, so those
/// two conditions arrive together. Aircraft.cpp gates on its pointer for the
/// same reason. Assets::releaseRamBuffer is null-safe, so this is safe to call
/// when nothing is held.
void releaseRam() {
  Assets::releaseRamBuffer(gRamBuffer);
  gRamItem = kNothingHeld;
}

/// Everything this card reaches the network for.
///
/// Called on the scheduler's refresh timer, never on a draw. ensureCached
/// returns at once for a picture already on the card, so the ordinary pass is
/// ten cheap checks; the first pass after a household changes their set is
/// where the fetches happen.
void fetch() {
  bool anyOnCard = false;

  for (uint8_t i = 0; i < gCount; ++i) {
    gOnCard[i] = Assets::ensureCached(String(gIds[i]));
    anyOnCard = anyOnCard || gOnCard[i];
  }

  for (uint8_t i = gCount; i < kMaxPhotos; ++i) {
    gOnCard[i] = false;
  }

  if (gCount == 0) {
    releaseRam();
  } else if (!anyOnCard) {
    // No SD card, or none of the set reached it. One photograph goes to heap
    // and the next pass takes the one after it, so the set cycles rather than
    // the display showing the same picture until it restarts.
    if (gRamNext >= gCount) {
      gRamNext = 0;
    }

    const uint8_t wanted = gRamNext;
    gRamNext = static_cast<uint8_t>((gRamNext + 1) % gCount);

    if (gRamItem != wanted) {
      releaseRam();

      if (Assets::fetchToRam(String(gIds[wanted]), gRamBuffer)) {
        gRamItem = wanted;
      } else {
        // releaseRam() above already handed back whatever fetchToRam had
        // committed before it failed.
        releaseRam();
      }
    }
  } else if (gRamItem != kNothingHeld) {
    // The card is back. Nothing should keep paying for a heap buffer it no
    // longer needs.
    releaseRam();
  }

  if (gCount != gLastLoggedCount) {
    if (gCount == 0) {
      Log::printf("[%s] no photographs chosen - the card will be skipped", kCardId);
    } else {
      Log::printf("[%s] holding %u photograph(s)", kCardId, static_cast<unsigned>(gCount));
    }
    gLastLoggedCount = gCount;
  }
}

uint16_t itemCount() { return gCount; }

void drawNoContent() {
  Display::showNoContent("No picture to show", "This photograph could not be loaded.");
}

/// A pure redraw. Never fetches: drawCached and drawRam are the draw-path
/// entry points, where drawFullScreen would reach for the network on a miss.
void draw(uint16_t itemIndex) {
  if (itemIndex >= gCount) {
    drawNoContent();
    return;
  }

  const uint8_t item = static_cast<uint8_t>(itemIndex);
  const char* const id = gIds[item];

  if (gOnCard[item]) {
    if (!Assets::drawCached(String(id))) {
      // The decode failed. drawImageFromSd has already cleared the panel, so
      // something has to go on it, and this item is dropped until the next
      // fetch so a picture this display cannot decode costs one dwell rather
      // than blanking the card on every cycle.
      gOnCard[item] = false;
      Log::printf("[%s] photograph '%s' did not decode", kCardId, id);
      drawNoContent();
    }
    return;
  }

  if (gRamItem == item) {
    if (!Assets::drawRam(String(id), gRamBuffer)) {
      Log::printf("[%s] photograph '%s' did not decode from RAM", kCardId, id);
      releaseRam();
      drawNoContent();
    }
    return;
  }

  // Not on the card and not the one in heap. Ordinary on a cardless display
  // holding one of several, and the next fetch moves the buffer along.
  drawNoContent();
}

bool registerSelf() {
  Cards::CardSpec spec;
  spec.id = kCardId;

  // List rather than Interstitial: a variable-length set cycled an item at a
  // time, which is the aircraft and calendar shape. Graphic is an interstitial
  // because it is one picture.
  spec.kind = Cards::Kind::List;
  spec.fetch = &fetch;
  spec.itemCount = &itemCount;
  spec.draw = &draw;

  // Behind the data cards and ahead of the decorative ones.
  spec.order = 4;
  spec.dwellSeconds = 10;
  return Cards::registerCard(spec);
}

[[maybe_unused]] const bool kRegistered = registerSelf();

}  // namespace

void setFromCheckIn(const char ids[][kAssetIdLength + 1], uint8_t count) {
  // Bounded here as well as in the parser and on the server. The parser stops
  // at kMaxPhotos, so this clamp cannot fire from that caller today; it is what
  // keeps a second caller from writing past the array.
  if (count > kMaxPhotos) {
    Log::printf("[%s] server sent %u photographs, keeping the first %u", kCardId,
                static_cast<unsigned>(count), static_cast<unsigned>(kMaxPhotos));
    count = kMaxPhotos;
  }

  // Decided before anything is written, because the old ids are what says which
  // cache files have to go and the copy below overwrites them. The short-circuit
  // also keeps the comparison off gIds slots past gCount, which hold whatever a
  // longer earlier set left there.
  bool changed = count != gCount;

  for (uint8_t i = 0; i < count && !changed; ++i) {
    changed = std::strcmp(gIds[i], ids[i]) != 0;
  }

  if (!changed) {
    // Every check-in carries the set and nearly all of them carry the same one.
    // Returning here is what keeps pictures the family still wants from being
    // deleted and re-fetched on the check-in cadence.
    return;
  }

  invalidateDeparted(ids, count);

  for (uint8_t i = 0; i < count; ++i) {
    std::strncpy(gIds[i], ids[i], kAssetIdLength);
    gIds[i][kAssetIdLength] = '\0';
  }

  gCount = count;

  // The set moved, so what is held and what is known to be on the card are both
  // about a set that no longer exists. fetch() rebuilds them on its next pass.
  for (uint8_t i = 0; i < kMaxPhotos; ++i) {
    gOnCard[i] = false;
  }

  releaseRam();
  gRamNext = 0;
}

uint16_t count() { return gCount; }

void releaseRamBuffer() { releaseRam(); }

}  // namespace PersonalPhotos
