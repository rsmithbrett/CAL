#include "Assets.h"

#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <SD.h>

#include "Config.h"
#include "Display.h"
#include "Identity.h"
#include "Log.h"
#include "SdStorage.h"
#include "Tls.h"

namespace Assets {
namespace {

/// The device-authenticated fetch route. The server's Assets domain is still
/// a stub, so this path is this firmware's expectation of it rather than
/// something that has ever answered - see Assets.h. Shaped like every other
/// device route the App calls: no id of the device anywhere in the request,
/// only the X-Device-Secret header (see MyWeatherEndpoints' "mine" route for
/// the fuller reasoning behind that convention).
constexpr const char* kFetchPath = "/api/assets/";

/// One directory, so cachedCount() below is a single readdir and a person
/// with the card in a reader can see exactly what a device has pulled down.
constexpr const char* kCacheDir = "/assets";

/// Everything is stored as PNG. The server normalises and re-encodes at
/// upload time (see Assets.h), so it decides the format and there is no
/// reason for a device to carry a decoder for every format someone might
/// upload. LovyanGFX's PNG support is what CYD-Dickey already relies on for
/// its own SD-hosted splash.
constexpr const char* kExtension = ".png";

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

/// Streams the response straight to the card rather than through a String -
/// an asset is tens of kilobytes and this device has roughly 274KB of free
/// heap, so buffering the whole body first is exactly the allocation that
/// would make a slightly-too-large image fatal instead of merely slow.
bool fetchToCard(const String& id) {
  NetworkClientSecure client;
  if (!Tls::configure(client)) {
    Log::line("[assets] TLS setup failed");
    return false;
  }

  HTTPClient http;
  const String url = String("https://") + Config::kServiceHost + kFetchPath + id;
  if (!http.begin(client, url)) {
    Log::printf("[assets] could not begin request for '%s'", id.c_str());
    return false;
  }
  http.setTimeout(Config::kHttpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());

  const int status = http.GET();
  if (status != 200) {
    http.end();
    Log::printf("[assets] fetch of '%s' failed, http status=%d", id.c_str(), status);
    return false;
  }

  // Written to a temporary name and renamed on success, so an interrupted
  // download (power loss, WiFi drop mid-body) can never leave a truncated
  // file that every later ensureCached() then treats as a cache hit.
  const String finalPath = pathFor(id);
  const String tempPath = finalPath + ".part";
  SD.remove(tempPath);
  File out = SD.open(tempPath, FILE_WRITE);
  if (!out) {
    http.end();
    Log::printf("[assets] could not open %s for writing", tempPath.c_str());
    return false;
  }

  const int written = http.writeToStream(&out);
  out.close();
  http.end();

  if (written <= 0) {
    SD.remove(tempPath);
    Log::printf("[assets] fetch of '%s' wrote nothing (%d)", id.c_str(), written);
    return false;
  }

  SD.remove(finalPath);
  if (!SD.rename(tempPath, finalPath)) {
    SD.remove(tempPath);
    Log::printf("[assets] could not move %s into place", tempPath.c_str());
    return false;
  }

  Log::printf("[assets] cached '%s' (%d bytes)", id.c_str(), written);
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
  return fetchToCard(id);
}

bool isCached(const String& id) {
  return Sd::isReady() && isSafeId(id) && SD.exists(pathFor(id));
}

bool drawFullScreen(const String& id) {
  if (!ensureCached(id)) {
    return false;
  }
  return Display::drawPngFromSd(pathFor(id));
}

bool drawCachedInRect(const String& id, int32_t x, int32_t y, int32_t w, int32_t h) {
  if (!isCached(id)) {
    return false;
  }
  return Display::drawPngFromSdInRect(pathFor(id), x, y, w, h);
}

bool drawCached(const String& id) {
  if (!isCached(id)) {
    return false;
  }
  return Display::drawPngFromSd(pathFor(id));
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
    if (!entry.isDirectory()) {
      count++;
    }
    entry.close();
  }
  dir.close();
  return count;
}

void showBootSplash() {
  if (!Sd::isReady()) {
    return;
  }
  const String path = pathFor("splash");
  if (!SD.exists(path)) {
    // Silently skipped, exactly like CYD-Dickey's own splash: a decoration
    // whose absence is the ordinary case for a device with no card.
    return;
  }
  Display::drawPngFromSd(path);
}

}  // namespace Assets
