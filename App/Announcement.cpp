#include "Announcement.h"

#include "Cards.h"
#include "Display.h"
#include "Log.h"

namespace Announcement {

const char* const kCardId = "announcement";
const char* const kCardId2 = "announcement2";
const char* const kCardId3 = "announcement3";
const char* const kCardId4 = "announcement4";
const char* const kCardId5 = "announcement5";

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
/// cross-check that lives on this side of the wire. One static_assert covers
/// all five instances below - they share the same descriptor shape, so there
/// is only one limit to check.
static_assert(Cards::kMaxTextLength == 280,
              "Cards::kMaxTextLength must match CardPolicyEditing.MaxTextLength on the server (280) - "
              "see the comment on each for why, and update both together");

/// One notice card's worth of itemCount/draw logic, parameterized on `N`
/// purely to give each instantiation its own registered id - the same
/// Instance<N> idiom Graphic.cpp established. Unlike Graphic there is no
/// per-instance cache or ready flag to hold: this card has no network fetch
/// at all (see the class remarks in Announcement.h), so the entire
/// implementation is a pure read of whatever `Cards::CardSpec::text` this
/// instance's own descriptor currently holds. That means every member below
/// except `id()` needs no static data at all - the template exists purely to
/// give five ids five distinct sets of static *functions*, each usable
/// directly as the plain function pointer `Cards::CardSpec` requires.
template <int N>
struct Instance {
  /// This instance's registered id. Defined only for N = 1 through 5 via the
  /// explicit specializations below - instantiating for any other N is a
  /// link error, the same guardrail Graphic.cpp's own id() has.
  static const char* id();

  /// The text the *current* policy carries, read back off this instance's
  /// own descriptor - deliberately not cached, the same reasoning the
  /// original single-instance currentText() gave: the policy can change
  /// under this module at any check-in, and re-reading it on every call is
  /// how both itemCount() and draw() stay honest about what is currently
  /// configured rather than what was configured when they last ran.
  ///
  /// Returns a pointer into the descriptor rather than a String for the same
  /// reason Graphic::Instance<N>::wantedAssetId() does: itemCount() and
  /// draw() both call this, the scheduler asks every card's itemCount() on
  /// nearly every tick, and there is no reason that question should cost a
  /// heap allocation. Never null, and only valid until the next
  /// applyPolicy() - every caller here uses it and drops it immediately.
  static const char* currentText() {
    const int8_t index = Cards::indexOf(id());
    if (index < 0) {
      return "";
    }
    return Cards::at(static_cast<uint8_t>(index)).text;
  }

  /// One item when this instance's policy currently has real text
  /// configured, zero otherwise - and zero is the ordinary state for every
  /// instance an admin has not typed a notice into yet. Mirrors
  /// Graphic::Instance<N>::itemCount()'s reasoning exactly, restated for
  /// text: there is no message worth putting on screen for "nobody wrote an
  /// announcement", so an unconfigured instance removes itself from the
  /// rotation entirely and the scheduler's existing empty-card skipping does
  /// the rest.
  static uint16_t itemCount() { return strlen(currentText()) > 0 ? 1 : 0; }

  /// Pure draw - reads this instance's own descriptor and hands the text
  /// straight to Display. There is no cache to be pure *about* (no network
  /// fetch happens anywhere in this module), so this is simply "draw
  /// whatever is configured right now", which is also exactly what reverse
  /// navigation needs: stepping back to this instance re-reads the same
  /// descriptor and draws the same words, never a fresh network answer.
  static void draw(uint16_t) {
    const char* const text = currentText();
    if (strlen(text) == 0) {
      // Only reachable if the policy changed between the scheduler's
      // itemCount() check and this call - see Graphic::Instance<N>::draw()'s
      // identical remark on its own equivalent guard.
      Display::showNoContent("No announcement to show", "This card has no text configured yet.");
      return;
    }
    Display::showAnnouncementCard(String(text));
  }

  /// Builds and registers this instance's descriptor. Called once per
  /// instantiation from the static-init block at the bottom of this file.
  static bool registerSelf(int16_t order, uint16_t interleaveEvery) {
    Cards::CardSpec spec;
    spec.id = id();
    spec.kind = Cards::Kind::Interstitial;
    // No `fetch` is assigned - unlike Graphic's instances, none of these
    // five has anything to refresh from the network. Content arrives already
    // complete on every check-in via CardManager::applyPolicy(), which
    // rewrites `text` directly on this instance's own descriptor whenever a
    // new policy names its id.
    spec.fetch = nullptr;
    spec.itemCount = &itemCount;
    spec.draw = &draw;
    spec.order = order;
    spec.dwellSeconds = 10;
    spec.interleaveEvery = interleaveEvery;
    return Cards::registerCard(spec);
  }
};

// The one piece of Instance<N> that cannot be written generically - each
// instance's id is a distinct string, not a function of N in any way the
// compiler could derive on its own.
template <>
const char* Instance<1>::id() {
  return kCardId;
}
template <>
const char* Instance<2>::id() {
  return kCardId2;
}
template <>
const char* Instance<3>::id() {
  return kCardId3;
}
template <>
const char* Instance<4>::id() {
  return kCardId4;
}
template <>
const char* Instance<5>::id() {
  return kCardId5;
}

// ---------------------------------------------------------------------------
// The card descriptors - five of them, one per Instance<N> instantiation.
//
// Registered at static-init time exactly like every other card module in
// this build - App.ino names no card, and adding these required no change
// to the scheduler at all.
//
// Interstitial, not list, for all five, the same reasoning as the original
// single-instance card: a notice is not a feed, and "show after every N
// other cards" is the honest description of how each one should appear in a
// rotation that also holds a variable-length aircraft list.
//
// All five share the same `order` (4) and `interleaveEvery` (8) - five peers
// of the same kind of card, not a priority chain; see Graphic::Instance<N>'s
// identical reasoning for why sharing a value here is harmless (the
// scheduler's own tie-break settles it by registration order).
//
// `text` has no built-in default and cannot have one for any instance: with
// no policy an instance simply reports zero items and never appears, the
// correct behaviour for an id nobody has written a notice for yet. A
// household that wants only one notice configures only `"announcement"` and
// never touches the other four ids; they then sit silent, exactly as
// `"announcement"` alone used to for a device with no policy at all.
// ---------------------------------------------------------------------------
[[maybe_unused]] const bool kRegistered1 = Instance<1>::registerSelf(4, 8);
[[maybe_unused]] const bool kRegistered2 = Instance<2>::registerSelf(4, 8);
[[maybe_unused]] const bool kRegistered3 = Instance<3>::registerSelf(4, 8);
[[maybe_unused]] const bool kRegistered4 = Instance<4>::registerSelf(4, 8);
[[maybe_unused]] const bool kRegistered5 = Instance<5>::registerSelf(4, 8);

}  // namespace

}  // namespace Announcement
