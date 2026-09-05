#include "Announcement.h"

#include "Cards.h"
#include "Display.h"
#include "Log.h"

namespace Announcement {

const char* const kCardId = "announcement";

namespace {

/// Cards::kMaxTextLength must equal DiscoverAroundMe.AdminUI.CardPolicyEditing
/// .MaxTextLength (280) on the server - that constant is what actually
/// enforces the bound at edit time, this one only sizes the fixed buffer the
/// value is carried in once it arrives. The two live in separate repositories
/// with no shared build to check them against each other, so this is the
/// closest thing to Graphic.cpp's own static_assert against
/// Assets::kMaxIdLength available here: it cannot catch the *server* number
/// changing, but it does catch this firmware's own number drifting from the
/// value both sides' comments say it must be, which is the half of the
/// cross-check that lives on this side of the wire.
static_assert(Cards::kMaxTextLength == 280,
              "Cards::kMaxTextLength must match CardPolicyEditing.MaxTextLength on the server (280) - "
              "see the comment on each for why, and update both together");

/// The text the *current* policy carries, read back off this card's own
/// descriptor - deliberately not cached in a global of its own, the same
/// reasoning Graphic.cpp's wantedAssetId() gives: the policy can change under
/// this module at any check-in, and re-reading it on every call is how both
/// itemCount() and draw() stay honest about what is currently configured
/// rather than what was configured when they last ran.
///
/// Unlike Graphic.cpp there is no cache to resolve this against - the text
/// arrives complete on the policy itself, so this is the entire "fetch"
/// this card needs, and it costs no network request at all. That is also why
/// this module registers no `fetch` function (see the registration block at
/// the bottom of this file): CardManager.cpp already tolerates a card with
/// `fetch == nullptr` (see its `refreshOneDueCard()`/`begin()` guards), and a
/// card with nothing to fetch should not pretend otherwise.
///
/// Returns a pointer into the descriptor rather than a String for the same
/// reason wantedAssetId() does: itemCount() and draw() both call this, the
/// scheduler asks every card's itemCount() on nearly every tick, and there is
/// no reason that question should cost a heap allocation. Never null, and
/// only valid until the next applyPolicy() - every caller here uses it and
/// drops it immediately.
const char* currentText() {
  const int8_t index = Cards::indexOf(kCardId);
  if (index < 0) {
    return "";
  }
  return Cards::at(static_cast<uint8_t>(index)).text;
}

/// One item when the policy currently has real text configured, zero
/// otherwise - and zero is the ordinary state for every device an admin has
/// not typed a notice into yet. Mirrors Graphic::cardItemCount()'s reasoning
/// exactly, restated for text: there is no message worth putting on screen
/// for "nobody wrote an announcement", so this card removes itself from the
/// rotation entirely and the scheduler's existing empty-card skipping does
/// the rest, rather than this module inventing a placeholder notice.
uint16_t cardItemCount() {
  return strlen(currentText()) > 0 ? 1 : 0;
}

/// Pure draw - reads the descriptor and hands the text straight to Display.
/// There is no cache to be pure *about* the way Graphic::cardDraw() has to
/// be (no network fetch happens anywhere in this module), so this is simply
/// "draw whatever is configured right now", which is also exactly what
/// reverse navigation needs: stepping back to this card re-reads the same
/// descriptor and draws the same words, never a fresh network answer.
void cardDraw(uint16_t) {
  const char* const text = currentText();
  if (strlen(text) == 0) {
    // Only reachable if the policy changed between the scheduler's
    // itemCount() check and this call - see Graphic::cardDraw()'s identical
    // remark on its own equivalent guard.
    Display::showNoContent("No announcement to show", "This card has no text configured yet.");
    return;
  }
  Display::showAnnouncementCard(String(text));
}

// ---------------------------------------------------------------------------
// The card descriptor.
//
// Registered at static-init time exactly like every other card module in
// this build - App.ino names no card, and adding this one required no change
// to the scheduler at all.
//
// Interstitial, not list, for the same reason Graphic.cpp's card is: one
// notice is not a feed, and "show after every N other cards" is the honest
// description of how a single thing should appear in a rotation that also
// holds a variable-length aircraft list.
//
// No `fetch` is assigned - unlike every other card in this build, this one
// has nothing to refresh from the network. Its content arrives already
// complete on every check-in via CardManager::applyPolicy(), which rewrites
// `text` directly on this descriptor whenever a new policy names this card.
//
// `text` has no built-in default and cannot have one: with no policy this
// card simply reports zero items and never appears, the correct behaviour
// for a device no admin has written a notice for yet. Ordered and interleaved
// alongside Graphic's picture card - both are admin-authored content with no
// data of their own to fall back on.
// ---------------------------------------------------------------------------
[[maybe_unused]] const bool kRegistered = [] {
  Cards::CardSpec spec;
  spec.id = kCardId;
  spec.kind = Cards::Kind::Interstitial;
  spec.fetch = nullptr;
  spec.itemCount = cardItemCount;
  spec.draw = cardDraw;
  spec.order = 4;
  spec.dwellSeconds = 10;
  spec.interleaveEvery = 8;
  return Cards::registerCard(spec);
}();

}  // namespace

}  // namespace Announcement
