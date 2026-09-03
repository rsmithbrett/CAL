#include "Graphic.h"

#include "Assets.h"
#include "Cards.h"
#include "Display.h"
#include "Log.h"

namespace Graphic {

const char* const kCardId = "graphic";

namespace {

/// The buffer a policy-supplied id is carried in must not be shorter than the
/// longest id the asset cache will accept, or an id inside the server's own
/// limit would arrive here truncated - a well-formed id for some other asset.
/// CardManager::applyPolicy() drops over-long ids rather than truncating them
/// precisely because of that; this makes a future divergence between the two
/// limits a compile error instead of a picture nobody can explain.
static_assert(Cards::kMaxAssetIdLength >= Assets::kMaxIdLength,
              "Cards::kMaxAssetIdLength must be able to hold any id Assets accepts");

/// The id this card successfully cached, and whether it is on the card right
/// now. Both are retained state, and the pair is what makes draw() a pure
/// redraw: draw() consults these and never asks the network anything.
String gCachedId;
bool gReady = false;

/// Logged only on a change of state. This runs on the refresh timer, so
/// logging unconditionally would put the same line in the remote debug stream
/// every ten minutes forever.
String gLastLoggedId;
bool gLastLoggedReady = false;

/// The asset the *current* policy wants, read back off this card's own
/// descriptor. Deliberately not cached in a global of its own: the policy can
/// change under this module at any check-in, and re-reading it is how both
/// fetch() and draw() stay honest about which picture is currently configured
/// rather than the one that was configured when they last ran.
///
/// Returns a pointer into the descriptor rather than a String because
/// itemCount() calls this several times per card switch (the scheduler asks
/// every card whether it is showable while it works out what comes next), and
/// there is no reason for that question to cost a heap allocation on a device
/// with roughly 274KB of it. Never null, and only valid until the next
/// applyPolicy() - every caller here uses it and drops it immediately.
const char* wantedAssetId() {
  const int8_t index = Cards::indexOf(kCardId);
  if (index < 0) {
    return "";
  }
  return Cards::at(static_cast<uint8_t>(index)).assetId;
}

void noteState(const char* id) {
  if (gReady == gLastLoggedReady && gLastLoggedId == id) {
    return;
  }
  gLastLoggedReady = gReady;
  gLastLoggedId = id;
  if (strlen(id) == 0) {
    Log::line("[graphic] no assetId in this card's policy - nothing to show");
  } else if (gReady) {
    Log::printf("[graphic] asset '%s' is on the card", id);
  } else {
    Log::printf("[graphic] asset '%s' is not available - the card will be skipped", id);
  }
}

/// Resolves the configured asset through the Assets cache: an SD hit costs a
/// stat, a miss costs one HTTP fetch that stores the file for every later
/// draw. Called only by the scheduler's refresh timer.
///
/// ensureCached() is called on every refresh rather than being skipped once
/// gReady is set, so a cache file that was deleted (a card swapped between
/// devices, a person tidying up the `/assets` directory) is noticed and
/// re-fetched instead of failing at draw time forever.
void cardFetch() {
  const char* const wanted = wantedAssetId();
  if (strlen(wanted) == 0) {
    // The ordinary state for a device nobody has configured a picture for.
    // Not an error, and not worth a network request.
    gReady = false;
    gCachedId = "";
    noteState(wanted);
    return;
  }

  gReady = Assets::ensureCached(String(wanted));
  gCachedId = gReady ? wanted : "";
  noteState(wanted);
}

/// One item when the picture the policy currently names is actually on the
/// card, zero otherwise - and zero is a perfectly ordinary answer here.
///
/// This is the opposite call from the one weather and aircraft make, on
/// purpose. Their non-Ok states are messages worth a screen ("weather is not
/// showing yet"), so they report one item and draw the message. A picture
/// that is missing has no message in it, so this card removes itself from the
/// rotation and the scheduler's existing empty-card skipping does the rest.
///
/// The comparison against wantedAssetId() is what makes a policy change take
/// effect immediately: the moment the server names a different asset, the one
/// this module holds stops counting as content, and it stays uncounted until
/// the next refresh has actually fetched the new one. Without it, a device
/// would keep showing the old picture for up to a full refresh interval after
/// being told to stop.
uint16_t cardItemCount() {
  if (!gReady || gCachedId.length() == 0) {
    return 0;
  }
  return gCachedId == wantedAssetId() ? 1 : 0;
}

/// The one no-content screen this card can put up. Reached only from draw(),
/// and only in the two cases itemCount() could not see coming - see both call
/// sites below. Routed through Display::showNoContent() rather than
/// showWeatherStatus()/showAircraftStatus(): those two carry their own card's
/// colour-banded banner, and this card has no banner of its own to wear.
void drawNoContent(const String& detail) {
  Display::showNoContent("No picture to show", detail);
}

/// Pure redraw. Never fetches - Assets::drawCached() is the draw-path entry
/// point precisely because Assets::drawFullScreen() would fetch on a miss,
/// and a rewind that reaches for the network is exactly what Cards.h's
/// fetch/draw split exists to prevent.
///
/// Display::drawPngFromSd() clears the panel to the day/night theme
/// background before decoding and centres the image on it, so the theme and
/// the centring are already handled and are the same ones every other screen
/// in this build uses. The corner clock and the action buttons are drawn by
/// CardManager after this returns (see drawChrome() in CardManager.cpp), the
/// same as for every other card - there is nothing card-specific to do here.
void cardDraw(uint16_t) {
  if (!gReady || gCachedId.length() == 0 || gCachedId != wantedAssetId()) {
    // Only reachable if the policy changed between the scheduler's
    // itemCount() check and this call. Falls through to the same no-content
    // screen as a failed decode below rather than leaving the panel blank.
    drawNoContent("This card has no image configured yet.");
    return;
  }

  if (Assets::drawCached(gCachedId)) {
    return;
  }

  // The decode failed, or the file went away between the check above and
  // here. drawPngFromSd() has already cleared the panel to the theme
  // background, so something has to go on it. Clearing gReady takes this card
  // straight back out of the rotation on the next computed card, so a
  // corrupt asset costs one dwell rather than reappearing every cycle.
  //
  // The cached file is deliberately NOT deleted. A PNG that will not decode
  // will not decode next time either, and deleting it would turn a permanent
  // failure into an HTTP fetch on every single refresh, forever - loud on the
  // network and no better on screen. It stays on the card, the failure is in
  // the log, and a new assetId is what fixes it.
  gReady = false;
  Log::printf("[graphic] asset '%s' would not decode - dropping this card from the rotation",
              gCachedId.c_str());
  drawNoContent("This card's image could not be displayed.");
}

// ---------------------------------------------------------------------------
// The card descriptor.
//
// Registered at static-init time exactly like Weather.cpp's and Aircraft.cpp's
// - App.ino names no card, and adding this one required no change to the
// scheduler at all, which is the property the registry in Cards.h exists to
// have.
//
// Interstitial, not list. `interleaveEvery` means "show after every N other
// cards", which is what a single picture wants: it appears on a cadence of its
// own no matter how many aircraft happen to be overhead. A list card would
// take one fixed slot in the list sequence and so be seen proportionally less
// often as that sequence grows - the specific mistake CardManager.h records
// having been corrected on a running device.
//
// Every value below is a built-in default that holds only until the first
// cardPolicy replaces it. `assetId` has no built-in default and cannot have
// one: with no policy this card simply reports zero items and never appears,
// which is the correct behaviour for a device nobody has given a picture to.
// Ordered after weather and aircraft, and interleaved less often than
// weather, so a decoration does not out-compete the data cards for screen
// time before a policy has an opinion.
// ---------------------------------------------------------------------------
[[maybe_unused]] const bool kRegistered = [] {
  Cards::CardSpec spec;
  spec.id = kCardId;
  spec.kind = Cards::Kind::Interstitial;
  spec.fetch = cardFetch;
  spec.itemCount = cardItemCount;
  spec.draw = cardDraw;
  spec.order = 3;
  spec.dwellSeconds = 10;
  spec.interleaveEvery = 8;
  return Cards::registerCard(spec);
}();

}  // namespace

}  // namespace Graphic
