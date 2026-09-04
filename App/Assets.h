#pragma once

#include <Arduino.h>

/// Images, cached on the SD card and addressed by server-side asset id.
///
/// The read path is: **SD hit -> draw. SD miss -> fetch from the server ->
/// store -> draw. Fetch failed -> draw nothing.** A miss must never block a
/// card: the card draws its text and the image simply is not there this time,
/// because a household staring at a frozen screen while a 40KB PNG times out
/// is a far worse outcome than a card with no picture on it.
///
/// Same storage approach as CYD-Dickey, which keeps its PNGs on SD and
/// addresses them by path (`splashImage = "/LRBH.PNG"`, see its SdCard.cpp
/// and showSplashScreen()). The difference here is that the path is derived
/// from a server-side id rather than typed in by a person, so the server's
/// asset catalog is the source of truth and a device populates its own cache
/// on demand.
///
/// **The device never validates image size.** The server normalises every
/// image at upload time - resized to a per-asset-type target, re-encoded,
/// metadata stripped - so what arrives here is already device-appropriate. A
/// device with roughly 274KB of free heap must never be the thing that
/// discovers an image was too big; that discovery belongs at upload, where a
/// person can see it.
///
/// **UNVERIFIED ON HARDWARE, and additionally unverified end-to-end:** the
/// server's asset catalog and its device-authenticated fetch endpoint are
/// still a stub at the time of writing, so `kFetchPath` below is this
/// firmware's expectation of that route rather than a route anything has
/// answered. PNG decode, SD writes and this fetch have all never run.
namespace Assets {

/// The longest id this module will accept. Ids come from the server, so this
/// is a sanity bound rather than a schema - see isSafeId() in Assets.cpp for
/// what it is guarding against. Cards::kMaxAssetIdLength must agree with it
/// (Graphic.cpp static_asserts that it does), since that is the buffer a
/// policy-supplied id is carried in.
static constexpr size_t kMaxIdLength = 48;

void begin();

/// True when the asset is on the card afterwards - either it already was, or
/// it was fetched and stored just now. False means "not available"; callers
/// draw nothing rather than waiting.
bool ensureCached(const String& id);

/// True when the asset is already on the card, with no network access of any
/// kind. This is the question a card's draw() may ask - fetching from a draw
/// path would make stepping backwards through the rotation a network
/// operation (see Cards.h on why fetch and draw are separate).
bool isCached(const String& id);

/// Draws the asset scaled to fit and centred on the whole panel. Returns
/// false when the asset is not available or the decode failed, having drawn
/// nothing.
///
/// **Fetches on a miss**, so this belongs on a fetch path and not in a
/// card's draw(); drawCached() below is the draw-path version.
bool drawFullScreen(const String& id);

/// Draws an asset that is already cached, and never fetches. False means
/// either "not on the card" (nothing was drawn) or "the decode failed", in
/// which case Display::drawPngFromSd() has already cleared the panel to the
/// theme background and the caller is expected to put its own no-content
/// state there. This is what a card's draw() calls.
bool drawCached(const String& id);

/// Same as drawCached(), bounded to a rectangle rather than the whole panel -
/// for a small logo layered onto a card another draw call already composed
/// (see Display::aircraftLogoZone()), rather than the picture being the
/// whole card the way Graphic.cpp's is. Never fetches, same reasoning as
/// drawCached().
bool drawCachedInRect(const String& id, int32_t x, int32_t y, int32_t w, int32_t h);

/// How many assets are currently cached - reported by Telemetry so the
/// fleet's storage view can show cache growth alongside sdUsedBytes.
uint16_t cachedCount();

/// Draws the "splash" asset at boot if it is already on the card, exactly the
/// way CYD-Dickey's showSplashScreen() does, and silently does nothing when
/// there is no card, no such asset, or the decode fails. Deliberately does
/// NOT fetch: boot is the one moment where waiting on the network to draw a
/// decoration is least defensible.
void showBootSplash();

}  // namespace Assets
