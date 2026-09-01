#include "Updater.h"

#include <ArduinoJson.h>
#include <FS.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <NetworkClientSecure.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>

#include "Config.h"
#include "Display.h"
#include "Identity.h"
#include "Tls.h"

namespace Updater {
namespace {

constexpr const char* kBrandSplashPath = "/brand.565";

bool beginSecure(NetworkClientSecure& client) {
  return Tls::configure(client);
}

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

const esp_partition_t* applicationPartition() {
  // The single OTA slot. Running from factory, this is always the target;
  // there is deliberately no second slot to alternate with.
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                  ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
}

}  // namespace

Manifest fetchManifest(const Service::Discovery& discovery) {
  Manifest out;

  NetworkClientSecure client;
  if (!beginSecure(client)) {
    return out;
  }

  HTTPClient http;
  const String url = String("https://") + Config::kServiceHost + discovery.manifestPath;
  if (!http.begin(client, url)) {
    return out;
  }
  http.setTimeout(discovery.httpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());

  const int status = http.GET();
  if (status != 200) {
    http.end();
    return out;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    return out;
  }

  // isConfigured false is a normal state, not a failure - it means no build has
  // been marked current on the server yet.
  out.isConfigured = doc["isConfigured"] | false;
  out.version = doc["version"] | "";
  out.sha256 = doc["sha256Hash"] | "";
  out.sizeBytes = doc["sizeBytes"] | 0;
  out.ok = true;
  return out;
}

bool installApplication(const Service::Discovery& discovery, const Manifest& manifest) {
  if (!manifest.isConfigured || manifest.sizeBytes == 0) {
    return false;
  }

  const esp_partition_t* target = applicationPartition();
  if (target == nullptr || manifest.sizeBytes > target->size) {
    // The server already refuses builds over the partition ceiling, so this is
    // a belt-and-braces check against a mismatched partition table.
    return false;
  }

  NetworkClientSecure client;
  if (!beginSecure(client)) {
    return false;
  }

  HTTPClient http;
  const String url = String("https://") + Config::kServiceHost + discovery.binaryPath;
  if (!http.begin(client, url)) {
    return false;
  }
  http.setTimeout(discovery.httpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());

  const int status = http.GET();
  if (status != 200) {
    http.end();
    return false;
  }

  if (!Update.begin(manifest.sizeBytes, U_FLASH)) {
    http.end();
    return false;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);

  NetworkClient* stream = http.getStreamPtr();
  uint8_t buffer[1024];
  uint32_t written = 0;
  uint8_t lastPercent = 255;

  while (http.connected() && written < manifest.sizeBytes) {
    const size_t available = stream->available();
    if (available == 0) {
      delay(1);
      continue;
    }

    const size_t toRead = available > sizeof(buffer) ? sizeof(buffer) : available;
    const int read = stream->readBytes(buffer, toRead);
    if (read <= 0) {
      continue;
    }

    if (Update.write(buffer, read) != static_cast<size_t>(read)) {
      Update.abort();
      mbedtls_sha256_free(&sha);
      http.end();
      return false;
    }
    mbedtls_sha256_update(&sha, buffer, read);
    written += read;

    const uint8_t percent = (written * 100) / manifest.sizeBytes;
    if (percent != lastPercent) {
      Display::showUpdateProgress(percent, manifest.version);
      lastPercent = percent;
    }
  }
  http.end();

  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);

  if (written != manifest.sizeBytes) {
    Update.abort();
    return false;
  }

  // Verified before the image is committed. An image that does not match the
  // manifest is discarded rather than marked bootable, so a truncated or
  // tampered download can never become the running application.
  if (!toHex(digest, sizeof(digest)).equalsIgnoreCase(manifest.sha256)) {
    Update.abort();
    return false;
  }

  if (!Update.end(true)) {
    return false;
  }

  Identity::setInstalledAppVersion(manifest.version);
  Identity::clearBootAttempts();
  Identity::setUpdateRequested(false);
  return true;
}

bool cacheBrandAssets(const Service::Discovery& discovery) {
  if (discovery.brandAssetPath.length() == 0) {
    return false;
  }

  NetworkClientSecure client;
  if (!beginSecure(client)) {
    return false;
  }

  HTTPClient http;
  const String url = String("https://") + Config::kServiceHost + discovery.brandAssetPath;
  if (!http.begin(client, url)) {
    return false;
  }
  http.setTimeout(discovery.httpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());

  if (http.GET() != 200) {
    http.end();
    return false;
  }

  // Written to a temporary name and renamed on success, so an interrupted
  // download cannot leave a half-written splash that renders as noise.
  const char* tmp = "/brand.tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) {
    http.end();
    return false;
  }

  const int written = http.writeToStream(&f);
  f.close();
  http.end();

  if (written <= 0) {
    LittleFS.remove(tmp);
    return false;
  }

  LittleFS.remove(kBrandSplashPath);
  return LittleFS.rename(tmp, kBrandSplashPath);
}

bool haveBootableApplication() {
  const esp_partition_t* app = applicationPartition();
  if (app == nullptr) {
    return false;
  }
  if (Identity::installedAppVersion().length() == 0) {
    return false;
  }
  // An application that has repeatedly failed to reach steady state is treated
  // as bad. CAL re-downloads rather than handing over to it again.
  return Identity::bootAttempts() < Identity::kMaxBootAttempts;
}

void bootApplication() {
  const esp_partition_t* app = applicationPartition();
  if (app == nullptr) {
    return;
  }

  Identity::recordBootAttempt();

  if (esp_ota_set_boot_partition(app) != ESP_OK) {
    return;
  }
  esp_restart();
}

}  // namespace Updater
