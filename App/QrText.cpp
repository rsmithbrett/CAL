#include "QrText.h"

#include "Cards.h"
#include "Display.h"
#include "Log.h"

namespace QrText {

const char* const kCardId = "qrtext";

namespace {

/// Cards::kMaxQrDataLength must equal DiscoverAroundMe.AdminUI.CardPolicyEditing
/// .MaxQrDataLength (100) on the server - that constant is what actually enforces the
/// bound at edit time (derived there from QR version 6's own byte-mode capacity), this one
/// only sizes the fixed buffer the value is carried in once it arrives. The two live in
/// separate repositories with no shared build to check them against each other, so this is
/// the same cross-check Announcement.cpp already does for kMaxTextLength: it cannot catch
/// the *server* number changing, but it does catch this firmware's own number drifting from
/// the value both sides' comments say it must be.
static_assert(Cards::kMaxQrDataLength == 100,
              "Cards::kMaxQrDataLength must match CardPolicyEditing.MaxQrDataLength on the server (100) - "
              "see the comment on each for why, and update both together");

/// The QR payload and caption the *current* policy carries, read back off this card's own
/// descriptor - deliberately not cached in globals of their own, the same reasoning
/// Announcement.cpp's currentText() gives: the policy can change under this module at any
/// check-in, and re-reading it on every call is how both itemCount() and draw() stay honest
/// about what is currently configured rather than what was configured when they last ran.
///
/// Returns a pointer into the descriptor for the same reason currentText() does: both
/// functions below call these, the scheduler asks every card's itemCount() on nearly every
/// tick, and there is no reason that question should cost a heap allocation. Never null,
/// and only valid until the next applyPolicy() - every caller here uses it and drops it
/// immediately.
const char* currentQrData() {
  const int8_t index = Cards::indexOf(kCardId);
  if (index < 0) {
    return "";
  }
  return Cards::at(static_cast<uint8_t>(index)).qrData;
}

const char* currentCaption() {
  const int8_t index = Cards::indexOf(kCardId);
  if (index < 0) {
    return "";
  }
  return Cards::at(static_cast<uint8_t>(index)).text;
}

/// One item when the policy currently has QR data configured, zero otherwise - and zero is
/// the ordinary state for every device an admin has not entered anything into yet. Mirrors
/// Announcement::cardItemCount()'s reasoning exactly, gated on `qrData` rather than `text`:
/// this card's caption is optional decoration (see QrText.h's own remarks on the two
/// fields' different roles), so a card with a caption but no data still has nothing to
/// encode and stays out of the rotation.
uint16_t cardItemCount() {
  return strlen(currentQrData()) > 0 ? 1 : 0;
}

/// Pure draw - reads the descriptor and hands both fields straight to Display. There is no
/// cache to be pure *about* (no network fetch happens anywhere in this module), so this is
/// simply "draw whatever is configured right now", which is also exactly what reverse
/// navigation needs: stepping back to this card re-reads the same descriptor and draws the
/// same code, never a fresh network answer.
void cardDraw(uint16_t) {
  const char* const qrData = currentQrData();
  if (strlen(qrData) == 0) {
    // Only reachable if the policy changed between the scheduler's itemCount()
    // check and this call - see Announcement::cardDraw()'s identical remark on
    // its own equivalent guard.
    Display::showNoContent("No QR code to show", "This card has no QR data configured yet.");
    return;
  }
  Display::showQrTextCard(String(qrData), String(currentCaption()));
}

// ---------------------------------------------------------------------------
// The card descriptor.
//
// Registered at static-init time exactly like every other card module in this
// build - App.ino names no card, and adding this one required no change to
// the scheduler at all.
//
// Interstitial, not list, for the same reason Announcement.cpp's card is: a
// single code is not a feed, and "show after every N other cards" is the
// honest description of how a single thing should appear in a rotation that
// also holds a variable-length aircraft list.
//
// No `fetch` is assigned - unlike every other card in this build except
// Announcement, this one has nothing to refresh from the network. Its
// content arrives already complete on every check-in via
// CardManager::applyPolicy(), which rewrites `qrData`/`text` directly on
// this descriptor whenever a new policy names this card.
//
// order/interleaveEvery: grouped with sunmoon (order 4, every 6), moonphase
// (order 4, every 7) and announcement (order 4, every 8) - all four are
// singleton facts or admin-authored content rather than a feed, so "show
// after every N other cards" describes all of them honestly, and order 4
// simply keeps this new one in that same tie-break group rather than
// inventing a fifth. interleaveEvery is 9 - one past announcement's 8 and
// one before clockdate's 10 - because scanning a QR code asks more of a
// person than reading a line of text (find a phone, open the camera, wait
// for it to focus): it earns a place in the rotation, but a slightly longer
// gap than the plain-text notice next to it is the honest reflection of
// that larger ask, while still appearing distinctly more often than the
// big clock face at 10, which nobody needs to interact with at all.
// ---------------------------------------------------------------------------
[[maybe_unused]] const bool kRegistered = [] {
  Cards::CardSpec spec;
  spec.id = kCardId;
  spec.kind = Cards::Kind::Interstitial;
  spec.fetch = nullptr;
  spec.itemCount = cardItemCount;
  spec.draw = cardDraw;
  spec.order = 4;
  spec.dwellSeconds = 12;
  spec.interleaveEvery = 9;
  return Cards::registerCard(spec);
}();

}  // namespace

}  // namespace QrText
