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
/// **Partially verified on hardware, the rest still is not.** A real device
/// reached the real server: TLS, `X-Device-Secret` auth, and the request
/// itself all worked, and came back with a real HTTP response - just the
/// wrong one, a 404, because the fetch path was never checked against the
/// endpoint actually built (`AssetsEndpoints.cs`'s `.../{id}/content`) and
/// silently diverged from it. That is now fixed. What is still genuinely
/// unverified is everything past a 200: no device has yet decoded a real
/// PNG or written one to its own SD card, because no fetch has ever
/// succeeded far enough to try.
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

/// The ids reported here failed to *decode* as a picture on this device -
/// present on the card, downloaded and SHA-256 verified fine, but LovyanGFX
/// still refused it (a corrupted PNG at the source, an encoding this
/// decoder cannot read). Recorded by drawCached()/drawCachedInRect()/
/// drawFullScreen() themselves whenever Display::drawPngFromSd*() returns
/// false for an id that was genuinely cached - so this can never fire for
/// the ordinary "hasn't fetched yet" case. Same "report what silently
/// failed on the next check-in" reasoning as CardManager's own
/// lastPolicyUnknownIds, and the same cap (4 ids, comma-joined) - see
/// CheckIn.cpp's own remarks for how this reaches the server. Deduplicated
/// per reporting cycle so one card redrawing the same bad asset every dwell
/// (Graphic.cpp's cards stop retrying after one failure, but an aircraft
/// logo overlay does not) cannot balloon the pending report.
uint8_t decodeFailureCount();
String decodeFailureIds();

/// Clears the pending decode-failure report - called once it has actually
/// been sent on a check-in, so a failure is reported once per occurrence
/// rather than on every subsequent check-in forever.
void clearDecodeFailures();

/// Draws the "splash" asset at boot if it is already on the card, exactly the
/// way CYD-Dickey's showSplashScreen() does, and silently does nothing when
/// there is no card, no such asset, or the decode fails. Deliberately does
/// NOT fetch: boot is the one moment where waiting on the network to draw a
/// decoration is least defensible.
void showBootSplash();

/// Deletes just this one asset's cached file, so the next ensureCached()
/// call for the same id re-fetches and re-verifies it from scratch. For a
/// card that has retried reading its already-cached file a few times and
/// still cannot decode it (see Graphic.cpp's draw()) - live evidence showed
/// this can be a transient SD read glitch rather than genuine file
/// corruption (the exact same bytes, pulled from the server and decoded
/// independently, were a perfectly well-formed PNG), so deleting the stale
/// copy and letting the very next refresh cycle fetch a fresh one recovers
/// from that glitch instead of blacklisting the card until someone changes
/// its assetId. A no-op if the id was never cached in the first place.
void invalidate(const String& id);

/// Deletes every cached asset (everything under the cache directory,
/// including a stray ".part" left by an interrupted download) so the next
/// ensureCached() for each one re-fetches and re-verifies from scratch.
///
/// Exists for the recovery path DeviceRegistry.RequestSdReformatAsync
/// triggers remotely (see App.ino's handling of CheckIn::Result::
/// sdReformatRequested): a card whose readback-verify keeps failing
/// (fetchToCard()'s "storage corrupted... this card may be failing" log
/// line) may have a corrupted FAT structure rather than genuinely dead
/// flash, and a clean directory to refetch into is worth trying before
/// physically swapping the card. This does NOT low-level format the
/// filesystem itself - the ESP32 core's SD library (SdFat under the hood)
/// exposes no such call through the SD.h wrapper this project uses - it
/// deletes every file this firmware ever wrote to the card, which is the
/// achievable, safe equivalent for what is, from this firmware's own
/// perspective, a single directory of files it fully owns. Returns the
/// number of files removed, purely for the debug log line the caller
/// prints - callers do not need to branch on it.
uint16_t wipeCache();

}  // namespace Assets
