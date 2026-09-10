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
/// **When there is no SD card at all** (missing, or a card that will not
/// mount even after Sd::begin()'s own retry), ensureCached() has nothing to
/// stat or write to and returns false immediately - "fetch failed" in the
/// path above, but for a reason no retry of the SD side will ever fix.
/// fetchToRam()/drawRam() below exist for exactly that case: a caller who
/// sees ensureCached() fail may fall back to fetching the same bytes
/// straight into RAM and drawing them from there, with no SD dependency at
/// all. It costs a network fetch every refresh instead of one ever - see
/// fetchToRam()'s own remarks for why that is an accepted tradeoff rather
/// than an oversight.
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

/// The one caller that cannot just call ensureCached(): a device's configured
/// boot-splash asset is chosen server-side per account (CheckInResponse.
/// SplashAssetId), and its actual bytes need to land specifically in the
/// fixed "splash" cache slot showBootSplash() below reads from - not in a
/// file named after `assetId`, which is what ensureCached(assetId) would
/// give it. Reuses the exact same SHA-256-verified, temp-file-then-rename
/// fetch as ensureCached() (see fetchToCard() in Assets.cpp), just aimed at a
/// different filename.
///
/// True when the "splash" slot already holds this exact asset id (tracked in
/// a small sidecar file, since the slot's own filename never changes) or it
/// was just fetched and stored there; false under the same conditions
/// ensureCached() returns false for. Cheap to call on every check-in that
/// reports a splash asset: the common case where the id has not changed does
/// no network I/O at all, only a sidecar read.
///
/// **Does not make a new or changed splash appear on screen.** showBootSplash()
/// only ever runs once, at boot, before this device's first check-in of the
/// run has happened - see its own remarks. Calling this after a check-in
/// only prepares the "splash" slot for the *next* boot; there is deliberately
/// no mid-session redraw here, since boot-time branding is the entire point
/// of this feature.
bool ensureSplashCached(const String& assetId);

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
/// which case Display::drawImageFromSd() has already cleared the panel to the
/// theme background and the caller is expected to put its own no-content
/// state there. This is what a card's draw() calls.
bool drawCached(const String& id);

/// Same as drawCached(), bounded to a rectangle rather than the whole panel -
/// for a small logo layered onto a card another draw call already composed
/// (see Display::aircraftLogoZone()), rather than the picture being the
/// whole card the way Graphic.cpp's is. Never fetches, same reasoning as
/// drawCached().
bool drawCachedInRect(const String& id, int32_t x, int32_t y, int32_t w, int32_t h);

/// A caller-owned, grow-only heap buffer for one direct-to-RAM asset fetch -
/// fetchToRam()'s/drawRam()'s equivalent of Display.cpp's own gFileBuffer,
/// but owned per-caller rather than shared globally. It has to be per-caller:
/// unlike an SD file (one independently-persisted copy per id, on the card),
/// a RAM fetch has nowhere else to live between Graphic.cpp's fetch() call
/// (which may run for several Instance<N>s back-to-back on the same refresh
/// tick) and its later draw() call (which can happen many times before the
/// next refresh). A single shared buffer would let one instance's fetch()
/// silently overwrite another's still-needed bytes; giving each caller its
/// own instance of this struct - exactly the way Graphic.cpp's Instance<N>
/// already gives each picture its own gCachedId/gReady - avoids that while
/// keeping the same "static, grow-only, never freed" discipline gFileBuffer
/// established: this is meant to live as a member of something that persists
/// for the process lifetime (a template's per-N static, in Graphic.cpp's
/// case), not to be constructed fresh per call.
struct RamAssetBuffer {
  uint8_t* data = nullptr;
  size_t capacity = 0;
  /// Valid bytes currently held. Only meaningful when the most recent
  /// fetchToRam() call into this buffer returned true; a caller must not
  /// read `data`/`size` after a failed call, since a failure can leave a
  /// partial write in place (see fetchToRam()'s own remarks).
  size_t size = 0;
};

/// The RAM-only sibling of ensureCached(): same HTTP GET, same
/// X-Device-Secret auth, same sha256 integrity check against the server's
/// X-Asset-Sha256 header as fetchToCard() (see Assets.cpp), but the response
/// body lands in `buffer` instead of an SD file - no SD access of any kind,
/// so this is what keeps "no picture" from being the only option on a device
/// whose SD card will not mount at all. `buffer` grows to fit (realloc,
/// never shrinks) and is the caller's to keep reusing across calls; this
/// function never frees it, even on failure, matching gFileBuffer's own
/// never-shrink policy in Display.cpp.
///
/// **Deliberately not persisted anywhere - this is the tradeoff that makes
/// the fallback possible, not a bug.** With no SD card there is nowhere to
/// cache the bytes, so whatever calls this pays the network cost again on
/// every refresh interval for as long as the fallback stays active, instead
/// of the usual one-fetch-ever an SD-backed asset costs. That is real,
/// ongoing network use this codebase does not normally ask of a "cached"
/// asset - accepted here because showing the picture at all, even at that
/// cost, is what "if it cannot do Graphics, it does not exist" requires, and
/// because a device already in this state has a working network connection
/// to spend it on (a card fetch happening at all is proof of that).
///
/// Returns false on any failure - TLS, non-200, a size/hash mismatch, or
/// running out of heap growing `buffer` - having written nothing the caller
/// should trust; `buffer.size` is only valid after a true return.
bool fetchToRam(const String& id, RamAssetBuffer& buffer);

/// Hands a RamAssetBuffer's memory back, against the "static, grow-only,
/// never freed" discipline every other buffer in this codebase follows.
///
/// **Why this one gets an exception.** Never-freeing is right when a buffer's
/// only competition is other allocations of its own kind. It is wrong here
/// because of what a RamAssetBuffer competes with: a new TLS session needs
/// roughly 32KB CONTIGUOUS, the largest 8-bit block on this board is capped
/// near 34,804 bytes once WiFi is up, and each graphic instance in RAM-
/// fallback mode holds its own ten-to-twenty-thousand-byte buffer for the
/// process lifetime. Two of those and there is no 32KB hole left anywhere.
///
/// That is measured, not inferred: the two devices with no SD card sit at
/// largest8 21,000-26,000 while drawing nothing at all, and their handshakes
/// fail with MBEDTLS_ERR_SSL_ALLOC_FAILED (-32512). A device in that state
/// cannot check in, cannot be sent a card policy, and cannot be given a
/// firmware update - so it is holding a picture at the price of being
/// manageable at all. Freeing the picture is the better trade.
///
/// Only ever called after a check-in has already failed, never speculatively:
/// on a healthy device the buffer is doing its job and costs nothing worth
/// reclaiming.
///
/// **A caller must also forget what it believed about the contents.** `size`
/// and `capacity` are zeroed here, but a caller holding its own "this id is
/// ready in RAM" flag has to clear that itself or it will hand a freed
/// pointer to the decoder. Graphic.cpp's releaseRamBuffers() is the worked
/// example.
void releaseRamBuffer(RamAssetBuffer& buffer);

/// Draws a buffer previously filled by fetchToRam(), with the same retry
/// count, decode-failure reporting and dedup as drawFullScreen()/drawCached()
/// give an SD-backed asset (see kMaxDrawAttempts in Assets.cpp) - a transient
/// decode glitch deserves the same second chance regardless of which path the
/// bytes arrived by. giveUpOnDecodeFailure()'s invalidate(id) call is a
/// harmless no-op here (there is nothing on SD to invalidate, and
/// invalidate() already early-returns when Sd::isReady() is false, which is
/// exactly the condition this path exists for) - reused as-is rather than
/// forked, so a RAM-fetched picture's decode failures show up in the same
/// decodeFailureIds()/decodeFailureCount() report CheckIn.cpp already sends.
bool drawRam(const String& id, const RamAssetBuffer& buffer);

/// How many assets are currently cached - reported by Telemetry so the
/// fleet's storage view can show cache growth alongside sdUsedBytes.
uint16_t cachedCount();

/// The ids reported here failed to *decode* as a picture on this device -
/// present on the card, downloaded and SHA-256 verified fine, but LovyanGFX
/// still refused it (a corrupted PNG at the source, an encoding this
/// decoder cannot read). Recorded by drawCached()/drawCachedInRect()/
/// drawFullScreen() themselves whenever Display::drawImageFromSd*() returns
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
///
/// The only writer of that slot is ensureSplashCached() above, called from
/// App.ino's performCheckIn() - and check-in only ever happens after this
/// function has already run once at boot. That means a freshly-configured
/// splash is invisible on the very boot that fetches it; it first draws on
/// the boot after. Not a bug to fix by triggering a redraw mid-session - see
/// ensureSplashCached()'s own remarks on why that is out of scope for what
/// this feature is actually for.
///
/// Returns whether a splash actually reached the screen. App.ino's setup()
/// uses that to decide whether to leave the logo up through WiFi join and
/// time sync, or to fall back to the ordinary "Checking the time"/"Loading"
/// status screens - a device with no splash must not be left staring at a
/// blank panel while the network comes up.
bool showBootSplash();

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
