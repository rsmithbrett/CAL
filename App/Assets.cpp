#include "Assets.h"

#include <SD.h>
#include <mbedtls/sha256.h>

#include "Config.h"
#include "Display.h"
#include "Http.h"
#include "Identity.h"
#include "Log.h"
#include "SdStorage.h"

namespace Assets {
namespace {

/// State for decodeFailureCount()/decodeFailureIds()/clearDecodeFailures() -
/// see Assets.h's own remarks on why this exists and how CheckIn.cpp uses it.
constexpr uint8_t kMaxReportedDecodeFailures = 4;
String gDecodeFailureIds;
uint8_t gDecodeFailureCount = 0;

/// How many times drawFullScreen()/drawCachedInRect()/drawCached() retry a
/// failed decode before giving up - centralised here (not duplicated per
/// card module) so every current and future caller gets the same retry,
/// invalidate-on-total-failure and decode-failure reporting for free. Found
/// live: a decode failure can be a transient read glitch rather than
/// genuine file corruption - see Assets.h's own remarks on invalidate().
constexpr uint8_t kMaxDrawAttempts = 3;
constexpr uint32_t kDrawRetryDelayMs = 75;

void recordDecodeFailure(const String& assetId) {
  // Dedup within one reporting cycle: an aircraft logo overlay redraws (and
  // can refail) every time that card is shown, and without this a single
  // bad logo would otherwise fill the whole 4-id cap with repeats of
  // itself before anything else ever got a slot.
  if (gDecodeFailureIds.indexOf(assetId) >= 0) {
    return;
  }
  if (gDecodeFailureCount < kMaxReportedDecodeFailures) {
    if (gDecodeFailureIds.length() > 0) {
      gDecodeFailureIds += ",";
    }
    gDecodeFailureIds += assetId;
  }
  gDecodeFailureCount++;
}

/// Every kMaxDrawAttempts-exhausted call site funnels through here, so the
/// "giving up on this one" log line exists exactly once regardless of which
/// caller (a full-screen picture, an in-rect overlay like the aircraft
/// card's airline logo, ...) hit it - found live that logging this only
/// from Graphic.cpp's own draw() left the aircraft logo's own decode
/// failures silent past the generic Display::drawPngFromSd*() line, the
/// same "loud, not silent" gap this whole mechanism exists to close.
void giveUpOnDecodeFailure(const String& id) {
  recordDecodeFailure(id);
  invalidate(id);
  Log::printf("[assets] '%s' would not decode after %u attempt(s) - invalidating the cache",
              id.c_str(), static_cast<unsigned>(kMaxDrawAttempts));
}

/// Same hex-encoding helper as CAL's own Updater.cpp uses to check a firmware
/// image's sha256 - duplicated rather than shared because App and CAL are
/// separate sketches with no common translation unit to hold it in.
String toHex(const uint8_t* bytes, size_t len) {
  static const char* kHex = "0123456789abcdef";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out += kHex[bytes[i] >> 4];
    out += kHex[bytes[i] & 0x0F];
  }
  return out;
}

/// The device-authenticated fetch route. Corrected against the real,
/// registered endpoint (`AssetsEndpoints.cs`: `MapGet("/api/assets/{id:guid}/content", ...)`)
/// after it produced a live 404 on real hardware: this constant used to read
/// "/api/assets/" with no trailing segment, an id concatenated directly onto
/// it, and no `/content` - written before the server side existed, as this
/// firmware's guess at what the route would look like, and never checked
/// against what was actually built. Shaped like every other device route the
/// App calls: no id of the device anywhere in the request, only the
/// X-Device-Secret header (see MyWeatherEndpoints' "mine" route for the
/// fuller reasoning behind that convention).
constexpr const char* kFetchPathPrefix = "/api/assets/";
constexpr const char* kFetchPathSuffix = "/content";

/// One directory, so cachedCount() below is a single readdir and a person
/// with the card in a reader can see exactly what a device has pulled down.
constexpr const char* kCacheDir = "/assets";

/// Everything is stored as PNG. The server normalises and re-encodes at
/// upload time (see Assets.h), so it decides the format and there is no
/// reason for a device to carry a decoder for every format someone might
/// upload. LovyanGFX's PNG support is what CYD-Dickey already relies on for
/// its own SD-hosted splash.
constexpr const char* kExtension = ".png";

/// The fixed cache-slot name showBootSplash() always reads from, regardless
/// of which catalog asset id an account has configured as its splash - see
/// ensureSplashCached() for why that filename can never be the asset's own
/// id the way every other cached asset's is.
constexpr const char* kSplashCacheId = "splash";

/// Refuses anything that would escape kCacheDir or confuse the filesystem.
/// Asset ids come from the server, which is trusted, but a path built by
/// string concatenation from a remote value deserves a guard regardless -
/// the failure mode otherwise is writing over something else on a card a
/// person also uses.
bool isSafeId(const String& id) {
  if (id.length() == 0 || id.length() > kMaxIdLength) {
    return false;
  }
  for (size_t i = 0; i < id.length(); ++i) {
    const char c = id.charAt(i);
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!allowed) {
      return false;
    }
  }
  return true;
}

String pathFor(const String& id) { return String(kCacheDir) + "/" + id + kExtension; }

/// Sidecar recording which asset id is currently behind the fixed "splash"
/// cache slot - see ensureSplashCached() below for why this needs recording
/// at all. A plain text file, not a second SD.exists() check: unlike every
/// other cached asset, "splash"'s filename never changes when the underlying
/// asset id does, so the filename itself carries no information about which
/// asset is currently sitting there.
String splashSourceIdPath() { return String(kCacheDir) + "/splash.id"; }

String readSplashSourceId() {
  File f = SD.open(splashSourceIdPath(), FILE_READ);
  if (!f) {
    return String();
  }
  String id = f.readString();
  f.close();
  id.trim();
  return id;
}

void writeSplashSourceId(const String& id) {
  SD.remove(splashSourceIdPath());
  File f = SD.open(splashSourceIdPath(), FILE_WRITE);
  if (!f) {
    // Non-fatal: the splash image itself is already on the card at this
    // point (fetchToCard() already succeeded, or this would never be
    // called). Losing this sidecar only costs one avoidable re-fetch the
    // next time the same asset id is still configured - it does not lose
    // the splash image, and does not stop showBootSplash() from drawing it.
    Log::line("[assets] could not write splash.id sidecar - next check-in will re-fetch unnecessarily");
    return;
  }
  f.print(id);
  f.close();
}

/// Grows a caller-owned RamAssetBuffer to at least `needed` bytes, exactly
/// the way Display.cpp's own ensureFileBufferCapacity() grows gFileBuffer -
/// realloc(), never shrinks, so a buffer that has already grown to fit the
/// largest asset a fallback-mode device has shown never pays another
/// allocation for anything smaller. Takes the buffer by reference rather
/// than operating on a single module-global precisely because it is not a
/// single module-global - see RamAssetBuffer's own remarks in Assets.h for
/// why each caller needs its own.
bool ensureRamBufferCapacity(RamAssetBuffer& buffer, size_t needed) {
  if (needed <= buffer.capacity) {
    return true;
  }
  uint8_t* grown = static_cast<uint8_t*>(realloc(buffer.data, needed));
  if (grown == nullptr) {
    return false;
  }
  buffer.data = grown;
  buffer.capacity = needed;
  return true;
}

/// Streams the response straight to the card rather than through a String -
/// an asset is tens of kilobytes and this device has roughly 274KB of free
/// heap, so buffering the whole body first is exactly the allocation that
/// would make a slightly-too-large image fatal instead of merely slow.
///
/// `fetchId` and `cacheId` are the same value for every ordinary caller
/// (ensureCached() below) - the asset's own id is both what the URL names and
/// what the cached filename is keyed by. ensureSplashCached() is the one
/// caller that splits them: it fetches whatever asset id the account has
/// configured but always stores the result under the fixed "splash" slot,
/// since that is the one filename showBootSplash() ever reads from and it has
/// no way to know at boot which asset id last won. Every other line of this
/// function - TLS, auth, streamed sha256, temp-file-then-rename, SD
/// readback-verify - stays identical for both callers.
bool fetchToCard(const String& fetchId, const String& cacheId) {
  if (!Http::ready()) {
    Log::line("[assets] TLS setup failed");
    return false;
  }

  const String url = String("https://") + Config::kServiceHost + kFetchPathPrefix + fetchId + kFetchPathSuffix;
  if (!Http::beginRequest(url)) {
    Log::printf("[assets] could not begin request for '%s'", fetchId.c_str());
    return false;
  }
  HTTPClient& http = Http::client();
  http.setTimeout(Config::kHttpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());

  Log::verbose("[assets] GET %s", url.c_str());
  const int status = http.GET();
  Log::verbose("[assets] fetch of '%s' response status=%d", fetchId.c_str(), status);
  if (status != 200) {
    http.end();
    Log::printf("[assets] fetch of '%s' failed, http status=%d", fetchId.c_str(), status);
    return false;
  }

  // Read before the body is consumed below - some HTTPClient paths report
  // getSize() as -1 once the stream has been read from, so both have to be
  // captured while the headers are still live. expectedHash is
  // AssetsEndpoints.cs's own X-Asset-Sha256 - the same value Assets'
  // ContentHash column holds, computed server-side from the exact bytes this
  // response carries.
  const int expectedSize = http.getSize();
  const String expectedHash = http.header("X-Asset-Sha256");

  // Written to a temporary name and renamed on success, so an interrupted
  // download (power loss, WiFi drop mid-body) can never leave a truncated
  // file that every later ensureCached() then treats as a cache hit. Keyed by
  // cacheId, not fetchId - see this function's own remarks above for why
  // those differ for ensureSplashCached().
  const String finalPath = pathFor(cacheId);
  const String tempPath = finalPath + ".part";
  SD.remove(tempPath);
  File out = SD.open(tempPath, FILE_WRITE);
  if (!out) {
    http.end();
    Log::printf("[assets] could not open %s for writing", tempPath.c_str());
    return false;
  }

  // Streamed with a running sha256 alongside the write, the same technique
  // Updater.cpp already uses to verify a firmware image, rather than
  // http.writeToStream()'s one-line copy: that copy only reports how many
  // bytes it attempted to hand SD.write(), the same count whether every byte
  // actually landed correctly or a marginal card silently dropped some of
  // them. Bug found live - a fresh re-upload of a known-good image, on a
  // device already running the size-check fix below, still failed to
  // decode, which a same-byte-count-but-wrong-content failure (network fine,
  // storage not) explains and a size check alone cannot catch.
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);

  NetworkClient* stream = http.getStreamPtr();
  uint8_t buffer[512];
  int written = 0;
  bool writeFailed = false;
  const uint32_t deadline = millis() + Config::kHttpTimeoutMs;

  while (http.connected() && (expectedSize < 0 || written < expectedSize)) {
    const size_t available = stream->available();
    if (available == 0) {
      if (millis() > deadline) {
        break;
      }
      delay(1);
      continue;
    }
    const size_t toRead = available > sizeof(buffer) ? sizeof(buffer) : available;
    const int read = stream->readBytes(buffer, toRead);
    if (read <= 0) {
      continue;
    }
    if (out.write(buffer, read) != static_cast<size_t>(read)) {
      writeFailed = true;
      break;
    }
    mbedtls_sha256_update(&sha, buffer, read);
    written += read;
  }
  out.close();
  http.end();

  if (writeFailed) {
    mbedtls_sha256_free(&sha);
    SD.remove(tempPath);
    Log::printf("[assets] fetch of '%s' - SD write failed after %d bytes", fetchId.c_str(), written);
    return false;
  }

  if (written <= 0) {
    mbedtls_sha256_free(&sha);
    SD.remove(tempPath);
    Log::printf("[assets] fetch of '%s' wrote nothing (%d)", fetchId.c_str(), written);
    return false;
  }

  // Not the same guarantee as written == the whole body: a connection that
  // drops mid-transfer still leaves a positive count for however much
  // arrived before it did. expectedSize is only checked when the server
  // actually sent a Content-Length (chunked responses report -1 here and
  // skip this check).
  if (expectedSize >= 0 && written != expectedSize) {
    mbedtls_sha256_free(&sha);
    SD.remove(tempPath);
    Log::printf("[assets] fetch of '%s' was truncated (wrote %d of %d bytes)", fetchId.c_str(),
                written, expectedSize);
    return false;
  }

  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);

  // The size check above only proves the network side delivered the right
  // byte count; it says nothing about whether those exact bytes are what
  // the server actually sent, or whether SD.write() silently corrupted some
  // of them on the way to flash - both invisible to a length comparison
  // alone, and both are what left a same-length file undecodable on real
  // hardware. expectedHash is only checked when the server actually sent it
  // (an older server without X-Asset-Sha256 skips this check, same
  // backward-compatibility posture as expectedSize above).
  if (expectedHash.length() > 0 && !toHex(digest, sizeof(digest)).equalsIgnoreCase(expectedHash)) {
    SD.remove(tempPath);
    Log::printf("[assets] fetch of '%s' failed integrity check (hash mismatch)", fetchId.c_str());
    return false;
  }

  // A second, independent check: re-read the bytes actually sitting in flash
  // and hash those, rather than trusting that a successful SD.write() call
  // means the card faithfully stored what it was given. The check above only
  // proves the in-RAM buffer matched before each write - a card silently
  // corrupting a sector on the way to flash (wear, a marginal card, a brief
  // power sag) would return success from write() and still be invisible to
  // it. Only run when the first check had something to verify against, for
  // the same backward-compatibility reason expectedHash's own check does.
  if (expectedHash.length() > 0) {
    File readBack = SD.open(tempPath, FILE_READ);
    if (!readBack) {
      SD.remove(tempPath);
      Log::printf("[assets] fetch of '%s' - could not reopen %s to verify what was actually "
                  "written",
                  fetchId.c_str(), tempPath.c_str());
      return false;
    }
    mbedtls_sha256_context verifySha;
    mbedtls_sha256_init(&verifySha);
    mbedtls_sha256_starts(&verifySha, 0);
    int readBackTotal = 0;
    while (readBack.available()) {
      const int read = readBack.read(buffer, sizeof(buffer));
      if (read <= 0) {
        break;
      }
      mbedtls_sha256_update(&verifySha, buffer, read);
      readBackTotal += read;
    }
    readBack.close();
    uint8_t verifyDigest[32];
    mbedtls_sha256_finish(&verifySha, verifyDigest);
    mbedtls_sha256_free(&verifySha);

    if (readBackTotal != written ||
        !toHex(verifyDigest, sizeof(verifyDigest)).equalsIgnoreCase(expectedHash)) {
      SD.remove(tempPath);
      Log::printf(
          "[assets] fetch of '%s' - storage corrupted what was written (network side verified "
          "fine, re-read from SD did not) - this card may be failing",
          fetchId.c_str());
      return false;
    }
  }

  SD.remove(finalPath);
  if (!SD.rename(tempPath, finalPath)) {
    SD.remove(tempPath);
    Log::printf("[assets] could not move %s into place", tempPath.c_str());
    return false;
  }

  // Names both ids when they differ (ensureSplashCached()'s case) so the log
  // reads "fetched X, stored as splash" rather than leaving which asset is
  // now behind the fixed "splash" slot to be inferred from a check-in
  // response minutes earlier.
  if (fetchId == cacheId) {
    Log::printf("[assets] cached '%s' (%d bytes, sha256 verified)", fetchId.c_str(), written);
  } else {
    Log::printf("[assets] fetched '%s', cached as '%s' (%d bytes, sha256 verified)",
                fetchId.c_str(), cacheId.c_str(), written);
  }
  return true;
}

/// The RAM-only sibling of fetchToCard() above - same request, same
/// X-Device-Secret header, same streamed sha256, but the destination is
/// `buffer` instead of a temp file on SD, and there is no rename-into-place
/// or SD readback-verify step, because there is nothing on SD to rename or
/// read back. See Assets.h's own remarks on fetchToRam() for why this
/// exists and the network-cost tradeoff it accepts.
bool fetchToRamImpl(const String& id, RamAssetBuffer& buffer) {
  if (!Http::ready()) {
    Log::line("[assets] TLS setup failed (direct-to-RAM fetch)");
    return false;
  }

  const String url = String("https://") + Config::kServiceHost + kFetchPathPrefix + id + kFetchPathSuffix;
  if (!Http::beginRequest(url)) {
    Log::printf("[assets] could not begin direct-to-RAM request for '%s'", id.c_str());
    return false;
  }
  HTTPClient& http = Http::client();
  http.setTimeout(Config::kHttpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());

  Log::verbose("[assets] GET %s (direct-to-RAM)", url.c_str());
  const int status = http.GET();
  Log::verbose("[assets] direct-to-RAM fetch of '%s' response status=%d", id.c_str(), status);
  if (status != 200) {
    http.end();
    Log::printf("[assets] direct-to-RAM fetch of '%s' failed, http status=%d", id.c_str(), status);
    return false;
  }

  // Same "read before the body is consumed" reasoning as fetchToCard() above.
  const int expectedSize = http.getSize();
  const String expectedHash = http.header("X-Asset-Sha256");

  // Grow up front when the server told us how big this is (the ordinary
  // case) - one allocation instead of one per 512-byte chunk below. A
  // chunked response with no Content-Length still works: ensureRamBufferCapacity()
  // is called again inside the loop as bytes actually arrive.
  if (expectedSize > 0 && !ensureRamBufferCapacity(buffer, static_cast<size_t>(expectedSize))) {
    http.end();
    Log::printf(
        "[assets] out of memory for a %d-byte direct-to-RAM fetch of '%s' (freeHeap=%u "
        "maxAllocHeap=%u)",
        expectedSize, id.c_str(), static_cast<unsigned>(ESP.getFreeHeap()),
        static_cast<unsigned>(ESP.getMaxAllocHeap()));
    return false;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);

  NetworkClient* stream = http.getStreamPtr();
  uint8_t chunk[512];
  size_t written = 0;
  bool outOfMemory = false;
  const uint32_t deadline = millis() + Config::kHttpTimeoutMs;

  while (http.connected() && (expectedSize < 0 || written < static_cast<size_t>(expectedSize))) {
    const size_t available = stream->available();
    if (available == 0) {
      if (millis() > deadline) {
        break;
      }
      delay(1);
      continue;
    }
    const size_t toRead = available > sizeof(chunk) ? sizeof(chunk) : available;
    const int read = stream->readBytes(chunk, toRead);
    if (read <= 0) {
      continue;
    }
    if (!ensureRamBufferCapacity(buffer, written + static_cast<size_t>(read))) {
      outOfMemory = true;
      Log::printf(
          "[assets] out of memory growing the direct-to-RAM buffer past %u bytes for '%s' "
          "(freeHeap=%u maxAllocHeap=%u)",
          static_cast<unsigned>(written), id.c_str(), static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
      break;
    }
    memcpy(buffer.data + written, chunk, static_cast<size_t>(read));
    mbedtls_sha256_update(&sha, chunk, static_cast<size_t>(read));
    written += static_cast<size_t>(read);
  }
  http.end();

  if (outOfMemory) {
    mbedtls_sha256_free(&sha);
    return false;
  }

  if (written == 0) {
    mbedtls_sha256_free(&sha);
    Log::printf("[assets] direct-to-RAM fetch of '%s' received nothing", id.c_str());
    return false;
  }

  // Same caveat as fetchToCard(): only checked when the server actually sent
  // a Content-Length, and only proves the byte count, not the content.
  if (expectedSize >= 0 && written != static_cast<size_t>(expectedSize)) {
    mbedtls_sha256_free(&sha);
    Log::printf("[assets] direct-to-RAM fetch of '%s' was truncated (got %u of %d bytes)",
                id.c_str(), static_cast<unsigned>(written), expectedSize);
    return false;
  }

  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);

  // No SD readback-verify step here, unlike fetchToCard(): there is nothing
  // written to storage to read back and re-hash. This sha256 check is
  // therefore only the network-side half of fetchToCard()'s two - it proves
  // the bytes now sitting in `buffer` are what the server actually sent, and
  // that is the entire guarantee a RAM-only path can offer, since there is
  // no storage step left afterwards for a card to silently corrupt.
  if (expectedHash.length() > 0 && !toHex(digest, sizeof(digest)).equalsIgnoreCase(expectedHash)) {
    Log::printf("[assets] direct-to-RAM fetch of '%s' failed integrity check (hash mismatch)",
                id.c_str());
    return false;
  }

  buffer.size = written;
  Log::printf("[assets] fetched '%s' straight to RAM (%u bytes, sha256 verified) - no SD "
              "dependency",
              id.c_str(), static_cast<unsigned>(written));
  return true;
}

}  // namespace

void begin() {
  if (!Sd::isReady()) {
    return;
  }
  if (!SD.exists(kCacheDir)) {
    SD.mkdir(kCacheDir);
  }
}

bool ensureCached(const String& id) {
  if (!Sd::isReady() || !isSafeId(id)) {
    return false;
  }
  if (SD.exists(pathFor(id))) {
    return true;
  }
  return fetchToCard(id, id);
}

bool ensureSplashCached(const String& assetId) {
  if (!Sd::isReady() || !isSafeId(assetId)) {
    return false;
  }
  // Unlike ensureCached() above, a hit is not "the file exists" - the
  // filename can't tell two different accounts' splash assets apart, since
  // it is always "splash", never the asset's own id. It has to be "the file
  // exists AND the sidecar says this is the asset currently behind it" -
  // otherwise a device that switched from asset A to asset B would see
  // splash.png already on disk, assume it was up to date, and keep showing A
  // forever.
  if (SD.exists(pathFor(kSplashCacheId)) && readSplashSourceId() == assetId) {
    return true;
  }
  if (!fetchToCard(assetId, kSplashCacheId)) {
    return false;
  }
  writeSplashSourceId(assetId);
  return true;
}

bool isCached(const String& id) {
  return Sd::isReady() && isSafeId(id) && SD.exists(pathFor(id));
}

bool fetchToRam(const String& id, RamAssetBuffer& buffer) {
  // isSafeId() here is not the path-traversal guard it is for the SD calls
  // above - there is no filesystem path in this function at all - but the
  // id still ends up concatenated into a URL and printed in log lines
  // below, so the same bound and character allowlist are worth keeping
  // rather than trusting an id that reached this far unchecked.
  if (!isSafeId(id)) {
    return false;
  }
  return fetchToRamImpl(id, buffer);
}

bool drawRam(const String& id, const RamAssetBuffer& buffer) {
  if (buffer.data == nullptr || buffer.size == 0) {
    return false;
  }
  for (uint8_t attempt = 0; attempt < kMaxDrawAttempts; ++attempt) {
    if (Display::drawPngFromBuffer(buffer.data, buffer.size)) {
      return true;
    }
    if (attempt + 1 < kMaxDrawAttempts) {
      delay(kDrawRetryDelayMs);
    }
  }
  giveUpOnDecodeFailure(id);
  return false;
}

void invalidate(const String& id) {
  if (!Sd::isReady() || !isSafeId(id)) {
    return;
  }
  SD.remove(pathFor(id));
}

bool drawFullScreen(const String& id) {
  if (!ensureCached(id)) {
    return false;
  }
  for (uint8_t attempt = 0; attempt < kMaxDrawAttempts; ++attempt) {
    if (Display::drawPngFromSd(pathFor(id))) {
      return true;
    }
    if (attempt + 1 < kMaxDrawAttempts) {
      delay(kDrawRetryDelayMs);
    }
  }
  giveUpOnDecodeFailure(id);
  return false;
}

bool drawCachedInRect(const String& id, int32_t x, int32_t y, int32_t w, int32_t h) {
  if (!isCached(id)) {
    return false;
  }
  for (uint8_t attempt = 0; attempt < kMaxDrawAttempts; ++attempt) {
    if (Display::drawPngFromSdInRect(pathFor(id), x, y, w, h)) {
      return true;
    }
    if (attempt + 1 < kMaxDrawAttempts) {
      delay(kDrawRetryDelayMs);
    }
  }
  giveUpOnDecodeFailure(id);
  return false;
}

bool drawCached(const String& id) {
  if (!isCached(id)) {
    return false;
  }
  for (uint8_t attempt = 0; attempt < kMaxDrawAttempts; ++attempt) {
    if (Display::drawPngFromSd(pathFor(id))) {
      return true;
    }
    if (attempt + 1 < kMaxDrawAttempts) {
      delay(kDrawRetryDelayMs);
    }
  }
  giveUpOnDecodeFailure(id);
  return false;
}

uint16_t cachedCount() {
  if (!Sd::isReady()) {
    return 0;
  }
  File dir = SD.open(kCacheDir);
  if (!dir || !dir.isDirectory()) {
    return 0;
  }
  uint16_t count = 0;
  for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    // splash.id is bookkeeping (see splashSourceIdPath()), not a cached
    // picture - counting it here would make Telemetry's cache-growth number
    // over-report by one for any device with a splash configured at all.
    const String name = entry.name();
    if (!entry.isDirectory() && name != "splash.id") {
      count++;
    }
    entry.close();
  }
  dir.close();
  return count;
}

uint8_t decodeFailureCount() { return gDecodeFailureCount; }

String decodeFailureIds() { return gDecodeFailureIds; }

void clearDecodeFailures() {
  gDecodeFailureIds = "";
  gDecodeFailureCount = 0;
}

void showBootSplash() {
  if (!Sd::isReady()) {
    return;
  }
  const String path = pathFor(kSplashCacheId);
  if (!SD.exists(path)) {
    // Silently skipped, exactly like CYD-Dickey's own splash: a decoration
    // whose absence is the ordinary case for a device with no card.
    return;
  }
  Display::drawPngFromSd(path);
}

uint16_t wipeCache() {
  if (!Sd::isReady()) {
    return 0;
  }
  File dir = SD.open(kCacheDir);
  if (!dir || !dir.isDirectory()) {
    return 0;
  }
  uint16_t removed = 0;
  for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    const String name = entry.name();
    const bool isDirectory = entry.isDirectory();
    entry.close();
    if (isDirectory) {
      continue;
    }
    // entry.name() is the bare filename (SD.open(kCacheDir) already put us
    // inside the directory), so the path passed to SD.remove() has to be
    // rebuilt with kCacheDir - the same join pathFor() does for a known id,
    // just for whatever name is actually sitting there rather than one this
    // firmware is about to construct itself.
    const String path = String(kCacheDir) + "/" + name;
    if (SD.remove(path)) {
      removed++;
    } else {
      Log::printf("[assets] wipeCache could not remove %s", path.c_str());
    }
  }
  dir.close();
  Log::printf("[assets] cache wiped (%u file(s) removed)", removed);
  return removed;
}

}  // namespace Assets
