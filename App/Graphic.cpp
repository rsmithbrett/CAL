#include "Graphic.h"

#include "Assets.h"
#include "Cards.h"
#include "Display.h"
#include "Log.h"

namespace Graphic {

const char* const kCardId = "graphic";
const char* const kCardId2 = "graphic2";
const char* const kCardId3 = "graphic3";
const char* const kCardId4 = "graphic4";
const char* const kCardId5 = "graphic5";

namespace {

/// The buffer a policy-supplied id is carried in must not be shorter than the
/// longest id the asset cache will accept, or an id inside the server's own
/// limit would arrive here truncated - a well-formed id for some other asset.
/// CardManager::applyPolicy() drops over-long ids rather than truncating them
/// precisely because of that; this makes a future divergence between the two
/// limits a compile error instead of a picture nobody can explain. One
/// static_assert covers all five instances below - they share the same
/// descriptor shape, so there is only one limit to check.
static_assert(Cards::kMaxAssetIdLength >= Assets::kMaxIdLength,
              "Cards::kMaxAssetIdLength must be able to hold any id Assets accepts");

/// One picture card's worth of fetch/itemCount/draw logic, parameterized on
/// `N` purely to give each instantiation its own set of static globals - the
/// same file-scope globals a single card would hold, replicated by the
/// compiler once per `N` instead of by hand five times in this file.
/// `Cards::CardSpec::fetch/itemCount/draw` are raw function pointers with no
/// per-instance context parameter (see Cards.h), which rules out a single
/// runtime class with an id data member: there would be nowhere to stash
/// `this` for the scheduler to pass back in. A template sidesteps that by
/// having the compiler generate five distinct sets of static functions and
/// statics instead, one per `N`, each usable directly as a plain function
/// pointer.
///
/// Only `id()` differs in a way that cannot be written once and reused - it
/// names a different string per instance - so it alone is explicitly
/// specialized for N = 1 through 5 below the class. Every other member here
/// is the single, shared implementation; instantiating this template five
/// times is what stands up five cards, not five copies of this logic.
template <int N>
struct Instance {
  /// This instance's registered id. Defined only for N = 1 through 5 via the
  /// explicit specializations below - instantiating for any other N is a
  /// link error, which is the intended guardrail against a copy-paste typo
  /// introducing a sixth instance without also giving it an id.
  static const char* id();

  /// The id this instance successfully cached, and whether it is on the card
  /// right now. Both are retained state, and the pair is what makes draw() a
  /// pure redraw: draw() consults these and never asks the network anything.
  /// One copy of each per instantiation - Instance<1>'s globals are as
  /// distinct from Instance<2>'s as if they had been declared in separate
  /// files.
  static String gCachedId;
  static bool gReady;

  /// Logged only on a change of state. This runs on the refresh timer, so
  /// logging unconditionally would put the same line in the remote debug
  /// stream every ten minutes forever, for every instance.
  static String gLastLoggedId;
  static bool gLastLoggedReady;

  /// The asset the *current* policy wants, read back off this instance's own
  /// descriptor. Deliberately not cached in a global of its own: the policy
  /// can change under this module at any check-in, and re-reading it is how
  /// both fetch() and draw() stay honest about which picture is currently
  /// configured rather than the one that was configured when they last ran.
  ///
  /// Returns a pointer into the descriptor rather than a String because
  /// itemCount() calls this several times per card switch (the scheduler
  /// asks every card whether it is showable while it works out what comes
  /// next), and there is no reason for that question to cost a heap
  /// allocation on a device with roughly 274KB of it. Never null, and only
  /// valid until the next applyPolicy() - every caller here uses it and
  /// drops it immediately.
  static const char* wantedAssetId() {
    const int8_t index = Cards::indexOf(id());
    if (index < 0) {
      return "";
    }
    return Cards::at(static_cast<uint8_t>(index)).assetId;
  }

  static void noteState(const char* assetId) {
    if (gReady == gLastLoggedReady && gLastLoggedId == assetId) {
      return;
    }
    gLastLoggedReady = gReady;
    gLastLoggedId = assetId;
    if (strlen(assetId) == 0) {
      Log::printf("[%s] no assetId in this card's policy - nothing to show", id());
    } else if (gReady) {
      Log::printf("[%s] asset '%s' is on the card", id(), assetId);
    } else {
      Log::printf("[%s] asset '%s' is not available - the card will be skipped", id(),
                   assetId);
    }
  }

  /// Resolves the configured asset through the Assets cache: an SD hit costs
  /// a stat, a miss costs one HTTP fetch that stores the file for every later
  /// draw. Called only by the scheduler's refresh timer.
  ///
  /// Called on every refresh rather than being skipped once gReady is set, so
  /// a cache file that was deleted (a card swapped between devices, a person
  /// tidying up the `/assets` directory) is noticed and re-fetched instead of
  /// failing at draw time forever.
  static void fetch() {
    const char* const wanted = wantedAssetId();
    if (strlen(wanted) == 0) {
      // The ordinary state for an instance nobody has configured a picture
      // for. Not an error, and not worth a network request.
      gReady = false;
      gCachedId = "";
      noteState(wanted);
      return;
    }

    gReady = Assets::ensureCached(String(wanted));
    gCachedId = gReady ? wanted : "";
    noteState(wanted);
  }

  /// One item when the picture this instance's policy currently names is
  /// actually on the card, zero otherwise - and zero is a perfectly ordinary
  /// answer here.
  ///
  /// This is the opposite call from the one weather and aircraft make, on
  /// purpose. Their non-Ok states are messages worth a screen ("weather is
  /// not showing yet"), so they report one item and draw the message. A
  /// picture that is missing has no message in it, so this instance removes
  /// itself from the rotation and the scheduler's existing empty-card
  /// skipping does the rest.
  ///
  /// The comparison against wantedAssetId() is what makes a policy change
  /// take effect immediately: the moment the server names a different asset,
  /// the one this instance holds stops counting as content, and it stays
  /// uncounted until the next refresh has actually fetched the new one.
  /// Without it, a device would keep showing the old picture for up to a full
  /// refresh interval after being told to stop.
  static uint16_t itemCount() {
    if (!gReady || gCachedId.length() == 0) {
      return 0;
    }
    return gCachedId == wantedAssetId() ? 1 : 0;
  }

  /// The one no-content screen this instance can put up. Reached only from
  /// draw(), and only in the two cases itemCount() could not see coming - see
  /// both call sites below. Routed through Display::showNoContent() rather
  /// than showWeatherStatus()/showAircraftStatus(): those two carry their own
  /// card's colour-banded banner, and a graphic card has no banner of its
  /// own to wear.
  static void drawNoContent(const String& detail) {
    Display::showNoContent("No picture to show", detail);
  }

  /// Pure redraw. Never fetches - Assets::drawCached() is the draw-path entry
  /// point precisely because Assets::drawFullScreen() would fetch on a miss,
  /// and a rewind that reaches for the network is exactly what Cards.h's
  /// fetch/draw split exists to prevent.
  ///
  /// Display::drawPngFromSd() clears the panel to the day/night theme
  /// background before decoding and centres the image on it, so the theme
  /// and the centring are already handled and are the same ones every other
  /// screen in this build uses. The corner clock and the action buttons are
  /// drawn by CardManager after this returns (see drawChrome() in
  /// CardManager.cpp), the same as for every other card - there is nothing
  /// card-specific to do here.
  static void draw(uint16_t) {
    if (!gReady || gCachedId.length() == 0 || gCachedId != wantedAssetId()) {
      // Only reachable if the policy changed between the scheduler's
      // itemCount() check and this call. Falls through to the same
      // no-content screen as a failed decode below rather than leaving the
      // panel blank.
      drawNoContent("This card has no image configured yet.");
      return;
    }

    // A few immediate retries before giving up. Found live: a decode
    // failure here can be a transient SD read glitch rather than genuine
    // file corruption - the exact same bytes, pulled from the server and
    // decoded independently off-device, turned out to be a perfectly
    // well-formed PNG. A short pause between attempts gives whatever caused
    // the glitch a chance to pass before the next read.
    constexpr uint8_t kMaxDrawAttempts = 3;
    for (uint8_t attempt = 0; attempt < kMaxDrawAttempts; ++attempt) {
      if (Assets::drawCached(gCachedId)) {
        return;
      }
      if (attempt + 1 < kMaxDrawAttempts) {
        delay(75);
      }
    }

    // Every attempt still failed. drawPngFromSd() has already cleared the
    // panel to the theme background, so something has to go on it. Clearing
    // gReady takes this instance straight back out of the rotation on the
    // next computed card, so a bad asset costs one dwell rather than
    // reappearing every cycle.
    //
    // Unlike the single-shot behaviour this replaced, the cached file is
    // now deleted (Assets::invalidate()) rather than kept and blacklisted
    // forever: that forces the very next fetch() refresh cycle to
    // re-download and re-verify a fresh copy, which is what actually
    // recovers from an intermittent read glitch rather than just giving up
    // on it once. A genuinely corrupt source image still ends up back here
    // on the next refresh and gets invalidated again - loud in the log and
    // (see Assets.h's decodeFailureCount()) in the /diag/audit
    // AssetDecodeFailure trail, not silent, but no longer a life sentence
    // for one bad read.
    gReady = false;
    Assets::invalidate(gCachedId);
    Log::printf(
        "[%s] asset '%s' would not decode after %u attempt(s) - invalidating the cache and "
        "dropping this card for now",
        id(), gCachedId.c_str(), static_cast<unsigned>(kMaxDrawAttempts));
    drawNoContent("This card's image could not be displayed.");
  }

  /// Builds and registers this instance's descriptor. Called once per
  /// instantiation from the static-init block at the bottom of this file -
  /// see that block for why `order`/`interleaveEvery` are the same across
  /// all three.
  static bool registerSelf(int16_t order, uint16_t interleaveEvery) {
    Cards::CardSpec spec;
    spec.id = id();
    spec.kind = Cards::Kind::Interstitial;
    spec.fetch = &fetch;
    spec.itemCount = &itemCount;
    spec.draw = &draw;
    spec.order = order;
    spec.dwellSeconds = 10;
    spec.interleaveEvery = interleaveEvery;
    return Cards::registerCard(spec);
  }
};

template <int N>
String Instance<N>::gCachedId;
template <int N>
bool Instance<N>::gReady = false;
template <int N>
String Instance<N>::gLastLoggedId;
template <int N>
bool Instance<N>::gLastLoggedReady = false;

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
// Registered at static-init time exactly like Weather.cpp's and Aircraft.cpp's
// - App.ino names no card, and adding these required no change to the
// scheduler at all, which is the property the registry in Cards.h exists to
// have.
//
// Interstitial, not list, for all five. `interleaveEvery` means "show after
// every N other cards", which is what a single picture wants: it appears on a
// cadence of its own no matter how many aircraft happen to be overhead. A
// list card would take one fixed slot in the list sequence and so be seen
// proportionally less often as that sequence grows - the specific mistake
// CardManager.h records having been corrected on a running CYD-Dickey device.
//
// All five share the same `order` (3) and `interleaveEvery` (8): they are
// five peers of the same kind of card, not a priority chain, and giving them
// distinct order values would only invent a meaningless ranking between five
// things a household picks independently. `order` still matters as the
// tie-break "two interstitials due on the same tick" case in Cards.h
// describes; sharing a value there just means the five graphic instances
// settle any such tie among themselves in registration order, which is as
// arbitrary - and as harmless - as any other tie-break would be.
//
// Every value below is a built-in default that holds only until the first
// cardPolicy replaces it, independently per instance. `assetId` has no
// built-in default and cannot have one: with no policy an instance simply
// reports zero items and never appears, which is the correct behaviour for
// an id nobody has given a picture to. Ordered after weather and aircraft,
// and interleaved less often than weather, so a decoration does not
// out-compete the data cards for screen time before a policy has an opinion.
//
// Five rather than three as of the multi-instance generalisation - see
// Cards.h's kMaxCards remarks and the judgment call in Announcement.h/
// QrText.h/Forecast.h for which other card types got the same treatment.
// ---------------------------------------------------------------------------
[[maybe_unused]] const bool kRegistered1 = Instance<1>::registerSelf(3, 8);
[[maybe_unused]] const bool kRegistered2 = Instance<2>::registerSelf(3, 8);
[[maybe_unused]] const bool kRegistered3 = Instance<3>::registerSelf(3, 8);
[[maybe_unused]] const bool kRegistered4 = Instance<4>::registerSelf(3, 8);
[[maybe_unused]] const bool kRegistered5 = Instance<5>::registerSelf(3, 8);

}  // namespace

}  // namespace Graphic
