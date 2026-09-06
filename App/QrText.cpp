#include "QrText.h"

#include "Cards.h"
#include "Display.h"
#include "Log.h"

namespace QrText {

const char* const kCardId = "qrtext";
const char* const kCardId2 = "qrtext2";
const char* const kCardId3 = "qrtext3";
const char* const kCardId4 = "qrtext4";
const char* const kCardId5 = "qrtext5";

namespace {

/// Cards::kMaxQrDataLength must equal DiscoverAroundMe.AdminUI.CardPolicyEditing
/// .MaxQrDataLength (100) on the server - that constant is what actually enforces the
/// bound at edit time (derived there from QR version 6's own byte-mode capacity), this one
/// only sizes the fixed buffer the value is carried in once it arrives. The two live in
/// separate repositories with no shared build to check them against each other, so this is
/// the same cross-check Announcement.cpp already does for kMaxTextLength: it cannot catch
/// the *server* number changing, but it does catch this firmware's own number drifting from
/// the value both sides' comments say it must be. One static_assert covers all five
/// instances below - they share the same descriptor shape, so there is only one limit to
/// check.
static_assert(Cards::kMaxQrDataLength == 100,
              "Cards::kMaxQrDataLength must match CardPolicyEditing.MaxQrDataLength on the server (100) - "
              "see the comment on each for why, and update both together");

/// One QR card's worth of itemCount/draw logic, parameterized on `N` purely to give each
/// instantiation its own registered id - the same Instance<N> idiom Graphic.cpp and
/// Announcement.cpp both use. No network fetch anywhere in this module (see the class
/// remarks in QrText.h), so - exactly like Announcement::Instance<N> - every member below
/// except `id()` needs no static data at all; it is a pure read of whatever
/// `Cards::CardSpec::qrData`/`.text` this instance's own descriptor currently holds.
template <int N>
struct Instance {
  /// This instance's registered id. Defined only for N = 1 through 5 via the explicit
  /// specializations below - instantiating for any other N is a link error, the same
  /// guardrail Graphic.cpp's own id() has.
  static const char* id();

  /// The QR payload and caption the *current* policy carries, read back off this
  /// instance's own descriptor - deliberately not cached, the same reasoning
  /// Announcement::Instance<N>::currentText() gives: the policy can change under this
  /// module at any check-in, and re-reading it on every call is how both itemCount() and
  /// draw() stay honest about what is currently configured rather than what was configured
  /// when they last ran.
  static const char* currentQrData() {
    const int8_t index = Cards::indexOf(id());
    if (index < 0) {
      return "";
    }
    return Cards::at(static_cast<uint8_t>(index)).qrData;
  }

  static const char* currentCaption() {
    const int8_t index = Cards::indexOf(id());
    if (index < 0) {
      return "";
    }
    return Cards::at(static_cast<uint8_t>(index)).text;
  }

  /// One item when this instance's policy currently has QR data configured, zero
  /// otherwise - and zero is the ordinary state for every instance an admin has not
  /// entered anything into yet. Mirrors Announcement::Instance<N>::itemCount()'s
  /// reasoning exactly, gated on `qrData` rather than `text`: this card's caption is
  /// optional decoration, so an instance with a caption but no data still has nothing to
  /// encode and stays out of the rotation.
  static uint16_t itemCount() { return strlen(currentQrData()) > 0 ? 1 : 0; }

  /// Pure draw - reads this instance's own descriptor and hands both fields straight to
  /// Display. There is no cache to be pure *about* (no network fetch happens anywhere in
  /// this module), so this is simply "draw whatever is configured right now", which is
  /// also exactly what reverse navigation needs.
  static void draw(uint16_t) {
    const char* const qrData = currentQrData();
    if (strlen(qrData) == 0) {
      // Only reachable if the policy changed between the scheduler's itemCount() check
      // and this call - see Announcement::Instance<N>::draw()'s identical remark on its
      // own equivalent guard.
      Display::showNoContent("No QR code to show", "This card has no QR data configured yet.");
      return;
    }
    Log::printf("[%s] drawing code for '%s'%s", id(), qrData,
                strlen(currentCaption()) > 0 ? " with caption" : "");
    Display::showQrTextCard(String(qrData), String(currentCaption()));
  }

  /// Builds and registers this instance's descriptor. Called once per instantiation from
  /// the static-init block at the bottom of this file.
  static bool registerSelf(int16_t order, uint16_t interleaveEvery) {
    Cards::CardSpec spec;
    spec.id = id();
    spec.kind = Cards::Kind::Interstitial;
    // No `fetch` is assigned - unlike Graphic's instances, none of these five has
    // anything to refresh from the network. Content arrives already complete on every
    // check-in via CardManager::applyPolicy(), which rewrites `qrData`/`text` directly on
    // this instance's own descriptor whenever a new policy names its id.
    spec.fetch = nullptr;
    spec.itemCount = &itemCount;
    spec.draw = &draw;
    spec.order = order;
    spec.dwellSeconds = 12;
    spec.interleaveEvery = interleaveEvery;
    return Cards::registerCard(spec);
  }
};

// The one piece of Instance<N> that cannot be written generically - each instance's id is
// a distinct string, not a function of N in any way the compiler could derive on its own.
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
// Interstitial, not list, for all five, the same reasoning as the original single-instance
// card: a single code is not a feed, and "show after every N other cards" is the honest
// description of how each one should appear.
//
// All five share the same `order` (4) and `interleaveEvery` (9) - five peers of the same
// kind of card, not a priority chain; see Graphic::Instance<N>'s identical reasoning for
// why sharing a value here is harmless.
//
// `qrData` has no built-in default for any instance: with no policy an instance simply
// reports zero items and never appears. A household that wants only one code configures
// only `"qrtext"` and never touches the other four ids; they then sit silent, exactly as
// `"qrtext"` alone used to for a device with no policy at all.
// ---------------------------------------------------------------------------
[[maybe_unused]] const bool kRegistered1 = Instance<1>::registerSelf(4, 9);
[[maybe_unused]] const bool kRegistered2 = Instance<2>::registerSelf(4, 9);
[[maybe_unused]] const bool kRegistered3 = Instance<3>::registerSelf(4, 9);
[[maybe_unused]] const bool kRegistered4 = Instance<4>::registerSelf(4, 9);
[[maybe_unused]] const bool kRegistered5 = Instance<5>::registerSelf(4, 9);

}  // namespace

}  // namespace QrText
