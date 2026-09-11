#include "Graphic.h"

#include "Assets.h"
#include "Cards.h"
#include "Display.h"
#include "HeapTrace.h"
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

  /// True when gCachedId's bytes came from fetchToRam() rather than the SD
  /// cache - set only when ensureCached() has already failed this cycle (no
  /// SD card, or a genuine cache-write failure) and the direct-to-RAM
  /// fallback below is what actually produced gReady. draw() reads this to
  /// decide whether to hand gCachedId to Assets::drawCached() (an SD read)
  /// or Assets::drawRam() (gRamBuffer, already in RAM) - drawing from the
  /// wrong one would either miss the picture entirely or, worse, try to
  /// read an SD file that was never written. See Assets.h's fetchToRam() for
  /// why this is not persisted anywhere and has to be re-earned every fetch().
  static bool gFromRam;

  /// This instance's own direct-to-RAM buffer, used only while gFromRam is
  /// true. One per instantiation, not one shared across all five - see
  /// Assets.h's RamAssetBuffer remarks for why a single shared buffer would
  /// let one instance's fallback fetch silently clobber another's still-
  /// showing picture. Static and grow-only like every other buffer this
  /// codebase added tonight: never freed for the process lifetime, so a
  /// device stuck in fallback mode pays at most one allocation per distinct
  /// size it has ever shown, not one per refresh.
  static Assets::RamAssetBuffer gRamBuffer;

  /// Logged only on a change of state. This runs on the refresh timer, so
  /// logging unconditionally would put the same line in the remote debug
  /// stream every ten minutes forever, for every instance.
  static String gLastLoggedId;
  static bool gLastLoggedReady;
  static bool gLastLoggedFromRam;

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
    if (gReady == gLastLoggedReady && gLastLoggedId == assetId &&
        gFromRam == gLastLoggedFromRam) {
      return;
    }
    gLastLoggedReady = gReady;
    gLastLoggedId = assetId;
    gLastLoggedFromRam = gFromRam;
    if (strlen(assetId) == 0) {
      Log::printf("[%s] no assetId in this card's policy - nothing to show", id());
    } else if (gReady && gFromRam) {
      // Distinct wording on purpose - see Assets.h's fetchToRam() remarks:
      // a remote debug-log observer should be able to tell "running without
      // SD" apart from the ordinary cached path at a glance, not have to
      // infer it from the absence of an SD-related line.
      Log::printf("[%s] asset '%s' is showing straight from RAM - no SD card available", id(),
                  assetId);
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
    gFromRam = false;
    if (!gReady) {
      // ensureCached() failed - either there is no SD card at all, or a
      // genuine cache-write failure. Either way the picture is not gone,
      // only its usual SD-cached path is - the bytes are still perfectly
      // fetchable over the network this device already reached the server
      // on, so try that straight into RAM before giving up entirely. See
      // Assets.h's fetchToRam() for the network-cost tradeoff this accepts
      // and Assets.RamAssetBuffer for why gRamBuffer is this instance's own
      // rather than shared.
      Log::printf(
          "[%s] SD cache unavailable for '%s' - falling back to a direct-to-RAM fetch (no SD "
          "dependency)",
          id(), wanted);
      gReady = Assets::fetchToRam(String(wanted), gRamBuffer);
      gFromRam = gReady;
      if (gReady) {
        Log::printf("[%s] direct-to-RAM fallback for '%s' succeeded (%u bytes) - showing without "
                    "SD",
                    id(), wanted, static_cast<unsigned>(gRamBuffer.size));
      } else {
        Log::printf("[%s] direct-to-RAM fallback for '%s' also failed - no picture this cycle",
                    id(), wanted);
      }
    }
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
  /// than showForecastStatus()/showAircraftStatus(): those two carry their own
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
  /// Display::drawImageFromSd() clears the panel to the day/night theme
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

    // What is actually on screen this draw, not just what fetch() last
    // resolved - the two can diverge across a rewind, where this runs again
    // with no fresh fetch behind it. noteState() above only logs on a
    // change of state, not on every draw.
    Log::verbose("[%s] drawing '%s'%s", id(), gCachedId.c_str(),
                gFromRam ? " (from RAM)" : " (from SD)");

    // gFromRam picks which of Assets' two draw entry points owns this
    // instance's bytes right now - drawCached() reads gCachedId back off
    // SD, drawRam() reads gRamBuffer, and using the wrong one for the
    // current state would either miss an SD-cached picture or try to read
    // an SD file the RAM fallback never wrote. Both give the same retry
    // count and decode-failure reporting (see Assets.h's drawRam() remarks).
    const bool drew =
        gFromRam ? Assets::drawRam(gCachedId, gRamBuffer) : Assets::drawCached(gCachedId);
    if (drew) {
      return;
    }

    // Assets::drawCached()/drawRam() already retried this same asset a few
    // times and logged the failure (see Assets.cpp's own
    // giveUpOnDecodeFailure()) - drawCached() additionally invalidated the
    // SD copy so the next fetch() re-downloads and re-verifies a fresh one
    // (see Assets.h's own remarks); drawRam() has no SD copy to invalidate,
    // and fetch() re-fetches into RAM every cycle regardless. All this
    // instance needs to do here is stop showing the stale picture: clearing
    // gReady takes it straight back out of the rotation on the next computed
    // card, so a bad asset costs one dwell rather than reappearing every
    // cycle, and it will reappear on its own once fetch() lands a working
    // copy.
    gReady = false;
    Log::printf("[%s] asset '%s' would not decode - dropping this card for now", id(),
                gCachedId.c_str());
    drawNoContent("This card's image could not be displayed.");
  }

  /// Gives this instance's RAM buffer back and forgets the picture it held.
  ///
  /// Clearing gReady/gFromRam/gCachedId is not tidying, it is the whole
  /// safety requirement - see Assets.h's releaseRamBuffer() remarks. gReady
  /// left true beside a freed pointer would send draw() to
  /// Assets::drawRam() with data that is no longer ours, and gCachedId left
  /// set would make the next fetch() believe the wanted asset was already
  /// held and skip the re-fetch, so the card would stay silent forever
  /// rather than for one refresh interval.
  ///
  /// Reporting zero items until the next successful fetch is the correct
  /// behaviour and not a regression: an instance with no picture is silent
  /// by design (see Graphic.h), and this is only ever reached on a device
  /// whose check-ins are already failing - which is also a device that
  /// cannot re-fetch anything until its connection comes back.
  /// Whether this instance is currently holding a RAM asset buffer. Read only
  /// by refreshGraphicsActiveCount() below, to put a per-device count on every
  /// HeapTrace line - a device with two graphic cards has two buffers live with
  /// interleaved cycles, and a trace without that count would read as
  /// non-deterministic.
  static bool holdsRamBuffer() { return gRamBuffer.data != nullptr; }

  static void releaseRamBuffer() {
    if (!gFromRam && gRamBuffer.data == nullptr) {
      return;
    }
    Assets::releaseRamBuffer(gRamBuffer);
    gReady = false;
    gFromRam = false;
    gCachedId = "";
    // Left deliberately untouched: gLastLoggedId/gLastLoggedReady/
    // gLastLoggedFromRam are the dedup state for noteState(), so clearing
    // them would make the next fetch log a "changed" line for a state that
    // had not changed. Their whole job is to survive across this.
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
bool Instance<N>::gFromRam = false;
template <int N>
Assets::RamAssetBuffer Instance<N>::gRamBuffer;
template <int N>
String Instance<N>::gLastLoggedId;
template <int N>
bool Instance<N>::gLastLoggedReady = false;
template <int N>
bool Instance<N>::gLastLoggedFromRam = false;

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

/// Counts the instances currently holding a RAM asset buffer and pushes the
/// figure into HeapTrace, so every trace line carries it.
///
/// Pushed rather than pulled: HeapTrace::mark() is called from Assets.cpp as
/// well, and having Assets reach into Graphic for one integer would invert the
/// module graph. See HeapTrace::setGraphicsActive().
void refreshGraphicsActiveCount() {
  const uint8_t active = static_cast<uint8_t>(
      (Instance<1>::holdsRamBuffer() ? 1 : 0) + (Instance<2>::holdsRamBuffer() ? 1 : 0) +
      (Instance<3>::holdsRamBuffer() ? 1 : 0) + (Instance<4>::holdsRamBuffer() ? 1 : 0) +
      (Instance<5>::holdsRamBuffer() ? 1 : 0));
  HeapTrace::setGraphicsActive(active);
}

}  // namespace

void refreshHeapTraceCounters() { refreshGraphicsActiveCount(); }

void releaseRamBuffers() {
  // Only an instance actually holding a RAM buffer has anything to give back;
  // an SD-backed or unconfigured one has a null pointer and
  // Assets::releaseRamBuffer() returns immediately. So on a device with a
  // working SD card this whole function is five null checks and produces no
  // log output at all, which is why the caller does not have to know whether
  // this device is in fallback mode.
  Instance<1>::releaseRamBuffer();
  Instance<2>::releaseRamBuffer();
  Instance<3>::releaseRamBuffer();
  Instance<4>::releaseRamBuffer();
  Instance<5>::releaseRamBuffer();
}

}  // namespace Graphic
