#include "Updater.h"

#include <ArduinoJson.h>
#include <FS.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <NetworkClientSecure.h>
#include <Update.h>
#include <esp_app_format.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>

#include "Config.h"
#include "Display.h"
#include "Identity.h"
#include "Journal.h"
#include "Tls.h"

namespace Updater {
namespace {

constexpr const char* kBrandSplashPath = "/brand.565";

/// Both numbers, never just the free figure.
///
/// `App/HeapRatchet` established that free bytes is the misleading one on this
/// hardware: mbedTLS needs 16,717 bytes CONTIGUOUS for each of a session's two
/// record buffers, and a device with 90KB free and a largest block of 11KB
/// fails the handshake while looking perfectly healthy. Devices 12 and 17 in the
/// 2026-09-11 incident were the two with badly fragmented heaps, so this is the
/// measurement that hypothesis stands or falls on.
void logHeap(const char* where) {
  Journal::printf("[heap] %s: free=%u largest8BitBlock=%u", where,
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

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

Manifest fetchManifest(const Service::Discovery& discovery, const String& pathOverride) {
  Manifest out;

  logHeap("before the manifest TLS session");

  NetworkClientSecure client;
  if (!beginSecure(client)) {
    Journal::line("[manifest] Tls::configure() refused - the embedded root bundle is "
                  "empty, so no request was made");
    return out;
  }

  HTTPClient http;
  const String path = pathOverride.length() > 0 ? pathOverride : discovery.manifestPath;
  const String url = String("https://") + Config::kServiceHost + path;
  if (!http.begin(client, url)) {
    Journal::printf("[manifest] http.begin() refused %s", url.c_str());
    return out;
  }
  http.setTimeout(discovery.httpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());

  const int status = http.GET();
  if (status != 200) {
    // Negative values are HTTPClient's own HTTPC_ERROR_* codes - connection
    // refused, TLS failure, DNS failure, timeout - not server status codes.
    Journal::printf("[manifest] GET %s returned %d", url.c_str(), status);
    http.end();
    return out;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Journal::printf("[manifest] response did not parse: %s", err.c_str());
    return out;
  }

  // isConfigured false is a normal state, not a failure - it means no build has
  // been marked current on the server yet.
  out.isConfigured = doc["isConfigured"] | false;
  out.version = doc["version"] | "";
  out.sha256 = doc["sha256Hash"] | "";
  out.sizeBytes = doc["sizeBytes"] | 0;
  out.ok = true;
  Journal::printf("[manifest] ok isConfigured=%d version='%s' size=%lu sha256=%.16s...",
                  out.isConfigured ? 1 : 0, out.version.c_str(),
                  static_cast<unsigned long>(out.sizeBytes), out.sha256.c_str());
  return out;
}

bool installApplication(const Service::Discovery& discovery, const Manifest& manifest,
                        bool asCalCandidate) {
  // This function is the reason the journal exists. Every early return below
  // used to be a silent `false` that surfaced on the panel as "Update failed",
  // and each one implies a different fix.
  if (!manifest.isConfigured || manifest.sizeBytes == 0) {
    Journal::printf("[install] refused before starting: isConfigured=%d sizeBytes=%lu",
                    manifest.isConfigured ? 1 : 0,
                    static_cast<unsigned long>(manifest.sizeBytes));
    return false;
  }

  const esp_partition_t* target = applicationPartition();
  if (target == nullptr) {
    Journal::line("[install] refused: no ota_0 partition in this device's table");
    return false;
  }

  // REFUSE TO INSTALL INTO THE PARTITION WE ARE EXECUTING FROM.
  //
  // Normally CAL runs from `factory` and ota_0 is somewhere else, so this cannot
  // happen. It happens when CAL is running from ota_0 itself - which is not
  // hypothetical, it is the whole mechanism of CAL_OTA_DESIGN.md section 5.4,
  // where a candidate CAL is downloaded into ota_0, booted there, and copies
  // itself into factory.
  //
  // Observed on device 17 on 2026-09-16, with CAL deliberately written to ota_0:
  //
  //   [install] Update.begin(1492144) failed: Partition Could Not be Found (error 10)
  //   [update] nothing bootable remains after the failed install
  //   [halt] Update failed | Restart the device to try again.
  //
  // Arduino's Update asks esp_ota_get_next_update_partition() for a slot that is
  // not the running one; on a table with a single ota_0 there is no answer, so it
  // fails with a message about a partition not being found. Accurate from its own
  // point of view and useless from here: the operator sees "Update failed" with
  // no hint that the device is running from the wrong place, and CAL halts.
  //
  // Checked BEFORE the download rather than after, which is the point. Reaching
  // Update.begin() means a 1.5MB TLS transfer has already been spent to learn
  // something knowable at the start, and on a metered or slow connection that is
  // not free.
  //
  // This refusal is not the feature - phase 2 of section 5.4 is, and it copies to
  // factory instead of downloading. Until that exists, refusing clearly beats
  // failing obscurely: the journal names the situation, and a device in this state
  // is one power cycle from normality because factory still holds a working CAL.
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running != nullptr && running->address == target->address) {
    Journal::printf("[install] refused: CAL is itself running from ota_0 at 0x%06lx, so "
                    "installing there would overwrite the code doing the installing. "
                    "factory still holds a bootable CAL - power-cycle to return to it. "
                    "See CAL_OTA_DESIGN.md 5.4: a CAL running from ota_0 must COPY "
                    "itself into factory, not download into its own partition",
                    static_cast<unsigned long>(running->address));
    return false;
  }

  if (manifest.sizeBytes > target->size) {
    // The server already refuses builds over the partition ceiling, so this is
    // a belt-and-braces check against a mismatched partition table.
    Journal::printf("[install] refused: image is %lu bytes and ota_0 holds %lu - this "
                    "device's partition table does not match the server's ceiling",
                    static_cast<unsigned long>(manifest.sizeBytes),
                    static_cast<unsigned long>(target->size));
    return false;
  }

  Journal::printf("[install] target ota_0 at 0x%06lx, %lu bytes; image '%s' is %lu bytes",
                  static_cast<unsigned long>(target->address),
                  static_cast<unsigned long>(target->size), manifest.version.c_str(),
                  static_cast<unsigned long>(manifest.sizeBytes));
  logHeap("before the download TLS session");

  NetworkClientSecure client;
  if (!beginSecure(client)) {
    Journal::line("[install] Tls::configure() refused - empty root bundle, nothing was "
                  "downloaded and the installed app is untouched");
    return false;
  }

  HTTPClient http;
  // A candidate fetches from the CAL route. Both land in ota_0 - that is the only
  // partition firmware can write - but what happens next differs entirely: an App
  // is handed control and keeps it, whereas a CAL image boots once as a candidate
  // and copies itself into factory. See SelfInstall.
  const String binPath = asCalCandidate ? discovery.calBinaryPath : discovery.binaryPath;
  const String url = String("https://") + Config::kServiceHost + binPath;
  if (!http.begin(client, url)) {
    Journal::printf("[install] http.begin() refused %s - the installed app is untouched",
                    url.c_str());
    return false;
  }
  http.setTimeout(discovery.httpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());

  const int status = http.GET();
  if (status != 200) {
    // A negative value here is an HTTPClient HTTPC_ERROR_* code, and -1 with a
    // heap figure alongside it is the signature of a handshake that could not
    // allocate its record buffers.
    Journal::printf("[install] GET %s returned %d - the installed app is untouched",
                    url.c_str(), status);
    logHeap("at the failed download request");
    http.end();
    return false;
  }
  logHeap("after the download response headers");

  // THE POINT OF NO RETURN. Update.begin() erases the target partition, so from
  // the next line onward the previously installed application no longer exists
  // and haveBootableApplication() will say so. Recorded explicitly because
  // "was the partition erased before or after the failure" was the single
  // biggest unknown in the 2026-09-11 incident, and is now a fact in the log
  // rather than an inference.
  Journal::line("[install] about to call Update.begin() - THIS ERASES ota_0 and the "
                "currently installed application stops existing at this point");

  if (!Update.begin(manifest.sizeBytes, U_FLASH)) {
    Journal::printf("[install] Update.begin(%lu) failed: %s (error %u)",
                    static_cast<unsigned long>(manifest.sizeBytes), Update.errorString(),
                    static_cast<unsigned>(Update.getError()));
    logHeap("at the failed Update.begin()");
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
  // Every 10%, not every 1%: the panel wants a smooth bar, the journal wants
  // ten lines rather than a hundred filling the boot's sector.
  uint8_t lastLoggedDecile = 255;

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
      Journal::printf("[install] Update.write() short at %lu of %lu bytes: %s (error %u)",
                      static_cast<unsigned long>(written),
                      static_cast<unsigned long>(manifest.sizeBytes),
                      Update.errorString(), static_cast<unsigned>(Update.getError()));
      logHeap("at the failed flash write");
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
    const uint8_t decile = percent / 10;
    if (decile != lastLoggedDecile) {
      Journal::printf("[install] %u%% - %lu of %lu bytes, freeHeap=%u largest8BitBlock=%u",
                      static_cast<unsigned>(percent), static_cast<unsigned long>(written),
                      static_cast<unsigned long>(manifest.sizeBytes),
                      static_cast<unsigned>(ESP.getFreeHeap()),
                      static_cast<unsigned>(
                          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
      lastLoggedDecile = decile;
    }
  }

  // Loop exit is not the same as success: `http.connected()` going false with
  // bytes outstanding is an aborted download, and the short-read check further
  // down is what catches it. Read BEFORE http.end(), which would make the
  // answer meaningless.
  const bool stillConnected = http.connected();
  http.end();

  Journal::printf("[install] download loop ended at %lu of %lu bytes (connected=%d)",
                  static_cast<unsigned long>(written),
                  static_cast<unsigned long>(manifest.sizeBytes),
                  stillConnected ? 1 : 0);

  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);

  if (written != manifest.sizeBytes) {
    Journal::printf("[install] SHORT DOWNLOAD: %lu of %lu bytes arrived - ota_0 is "
                    "erased and nothing bootable is in it",
                    static_cast<unsigned long>(written),
                    static_cast<unsigned long>(manifest.sizeBytes));
    Update.abort();
    return false;
  }

  // Verified before the image is committed. An image that does not match the
  // manifest is discarded rather than marked bootable, so a truncated or
  // tampered download can never become the running application.
  const String computed = toHex(digest, sizeof(digest));
  if (!computed.equalsIgnoreCase(manifest.sha256)) {
    // Both digests, because "the hash was wrong" and "the manifest carried a
    // hash for a different artifact" are different faults with the same symptom.
    Journal::printf("[install] SHA-256 MISMATCH after a complete %lu-byte download",
                    static_cast<unsigned long>(written));
    Journal::printf("[install]   computed=%s", computed.c_str());
    Journal::printf("[install]   manifest=%s", manifest.sha256.c_str());
    Update.abort();
    return false;
  }

  if (!Update.end(true)) {
    Journal::printf("[install] Update.end() refused to commit a hash-verified image: %s "
                    "(error %u)",
                    Update.errorString(), static_cast<unsigned>(Update.getError()));
    return false;
  }

  if (asCalCandidate) {
    // RECORD WHAT IS NOW SITTING IN ota_0, and record nothing about the App - the
    // App's version in nvs still describes whatever was in ota_0 before this
    // overwrote it, and that is now a lie. It is left alone deliberately: the
    // trampoline erases ota_0's header on the next boot, after which CAL sees no
    // bootable app and downloads one, which rewrites it correctly. Correcting it
    // here would only make the intervening boot look tidier.
    //
    // A CAL UPDATE THEREFORE COSTS AN APP RE-DOWNLOAD. ota_0 is the staging area
    // and there is nowhere else - CAL_OTA_DESIGN.md rejected carving a staging
    // partition on arithmetic, short by 404,752 bytes on this table. So a CAL
    // update is roughly 3MB of traffic and two reboots rather than one. That is
    // acceptable because CAL changes rarely, and it is why this is gated on an
    // operator marking a CAL current rather than on a version differing.
    Identity::recordCalCandidate(manifest.sizeBytes, manifest.sha256, manifest.version);
    Identity::clearBootAttempts();
    Journal::printf("[install] committed CAL candidate '%s' into ota_0 (%lu bytes, sha256 "
                    "recorded). The next boot runs it from there, and its only job will be "
                    "to copy itself into factory - see SelfInstall",
                    manifest.version.c_str(),
                    static_cast<unsigned long>(manifest.sizeBytes));
    return true;
  }

  Identity::setInstalledAppVersion(manifest.version);
  Identity::clearBootAttempts();
  Identity::setUpdateRequested(false);
  Journal::printf("[install] committed '%s' - boot attempts and the update-requested "
                  "flag are cleared",
                  manifest.version.c_str());
  return true;
}

bool cacheBrandAssets(const Service::Discovery& discovery) {
  if (discovery.brandAssetPath.length() == 0) {
    Journal::line("[brand] discovery named no brand asset path - skipped, and this is "
                  "normal for a device not yet assigned to a brand");
    return false;
  }

  NetworkClientSecure client;
  if (!beginSecure(client)) {
    Journal::line("[brand] Tls::configure() refused - splash not refreshed (cosmetic, "
                  "the boot continues)");
    return false;
  }

  HTTPClient http;
  const String url = String("https://") + Config::kServiceHost + discovery.brandAssetPath;
  if (!http.begin(client, url)) {
    Journal::printf("[brand] http.begin() refused %s (cosmetic)", url.c_str());
    return false;
  }
  http.setTimeout(discovery.httpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());

  const int status = http.GET();
  if (status != 200) {
    Journal::printf("[brand] GET returned %d - splash not refreshed (cosmetic)", status);
    http.end();
    return false;
  }

  // Written to a temporary name and renamed on success, so an interrupted
  // download cannot leave a half-written splash that renders as noise.
  const char* tmp = "/brand.tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) {
    Journal::line("[brand] could not open /brand.tmp for writing (cosmetic)");
    http.end();
    return false;
  }

  const int written = http.writeToStream(&f);
  f.close();
  http.end();

  if (written <= 0) {
    Journal::printf("[brand] wrote %d bytes - discarding the temporary file (cosmetic)",
                    written);
    LittleFS.remove(tmp);
    return false;
  }

  LittleFS.remove(kBrandSplashPath);
  const bool renamed = LittleFS.rename(tmp, kBrandSplashPath);
  Journal::printf("[brand] cached %d bytes to %s (rename %s)", written, kBrandSplashPath,
                  renamed ? "ok" : "FAILED");
  return renamed;
}

/// Whether `ota_0` actually begins with an ESP32 image header.
///
/// One byte, read out of flash. It exists because every other question this file
/// asks about the installed application is answered from nvs, and nvs is
/// bookkeeping ABOUT the flash rather than the flash itself. The two disagree
/// more often than the design assumed, and both known cases were expensive:
///
///   - 2026-09-15: a recovery attempt wrote an App image 262,144 bytes past
///     where the bootloader looks. nvs went on naming a version that was no
///     longer anywhere on the chip.
///   - 2026-09-16: SelfInstall::cleanUpAfterCandidate() erased ota_0's header on
///     purpose and left nvs alone on purpose, on the written assumption that the
///     ladder would then see no bootable app and download one. It did not. CAL
///     announced "bootable app 'v2026.09.16.0003' present", handed over, and the
///     ROM rejected the image CAL had erased seconds earlier.
///
/// The second one is why this reads. nvs can only say what CAL last believed;
/// the magic byte says what the bootloader is going to find.
static bool otaPartitionHoldsAnImage(const esp_partition_t* app) {
  uint8_t magic = 0;
  const esp_err_t err = esp_partition_read(app, 0, &magic, sizeof(magic));
  if (err != ESP_OK) {
    Journal::printf("[updater] could not read ota_0's first byte (esp_err %d) - treating it "
                    "as holding nothing, which costs a download and risks nothing",
                    static_cast<int>(err));
    return false;
  }
  return magic == ESP_IMAGE_HEADER_MAGIC;
}

bool haveBootableApplication() {
  // Four separate reasons, each logged as itself. "No bootable application"
  // covers a device that has never had one, a device whose install was
  // invalidated, a device whose partition does not hold what nvs claims, and a
  // device whose app crashes on startup - and those want four different
  // responses from whoever is reading the journal.
  const esp_partition_t* app = applicationPartition();
  if (app == nullptr) {
    Journal::line("[updater] no bootable app: this device's table has no ota_0 partition");
    return false;
  }
  const String version = Identity::installedAppVersion();
  if (version.length() == 0) {
    Journal::line("[updater] no bootable app: ota_0 exists but no installed version is "
                  "recorded, so nothing has ever been installed in it");
    return false;
  }
  // ASKED BEFORE THE BOOT-ATTEMPT COUNTER, and the order is the fix rather than a
  // detail. An empty partition is not a flaky application and must not be made to
  // look like one: with the counter first, a device with nothing in ota_0 has to
  // burn three halted boots - two of them requiring a human to reach over and
  // power-cycle - before CAL will consider downloading. Observed on device 17 on
  // 2026-09-16, sitting at "Cannot start application" with an erased ota_0 and a
  // working network it had decided not to touch.
  if (!otaPartitionHoldsAnImage(app)) {
    Journal::printf("[updater] no bootable app: nvs records '%s' but ota_0 does not begin "
                    "with an image header, so there is nothing at 0x%06lx to hand over to - "
                    "nvs was describing an install that is no longer on the chip",
                    version.c_str(), static_cast<unsigned long>(app->address));
    return false;
  }
  const uint8_t attempts = Identity::bootAttempts();
  if (attempts >= Identity::kMaxBootAttempts) {
    // An application that has repeatedly failed to reach steady state is treated
    // as bad. CAL re-downloads rather than handing over to it again.
    Journal::printf("[updater] no bootable app: '%s' has used %u of %u boot attempts "
                    "without reporting itself healthy, so it is being treated as bad",
                    version.c_str(), static_cast<unsigned>(attempts),
                    static_cast<unsigned>(Identity::kMaxBootAttempts));
    return false;
  }
  Journal::printf("[updater] bootable app '%s' present, %u of %u boot attempts used",
                  version.c_str(), static_cast<unsigned>(attempts),
                  static_cast<unsigned>(Identity::kMaxBootAttempts));
  return true;
}

void bootApplication(const char* describedAs) {
  const esp_partition_t* app = applicationPartition();
  if (app == nullptr) {
    Journal::line("[updater] handover abandoned: no ota_0 partition to hand over to");
    return;
  }

  // nvs only when the caller did not say. See the header: during a CAL update the
  // App version in nvs describes an image the candidate has already overwritten.
  const String what = describedAs != nullptr
                          ? String(describedAs)
                          : "the installed App '" + Identity::installedAppVersion() + "'";

  Identity::recordBootAttempt();
  Journal::printf("[updater] handing over to %s at 0x%06lx - boot attempts now %u of %u",
                  what.c_str(), static_cast<unsigned long>(app->address),
                  static_cast<unsigned>(Identity::bootAttempts()),
                  static_cast<unsigned>(Identity::kMaxBootAttempts));

  const esp_err_t err = esp_ota_set_boot_partition(app);
  if (err != ESP_OK) {
    Journal::printf("[updater] esp_ota_set_boot_partition failed (esp_err %d) - handover "
                    "abandoned, returning to CAL's ladder",
                    static_cast<int>(err));
    return;
  }

  // Written to flash line by line rather than buffered, so this really is on
  // the chip before the restart takes the RAM with it.
  Journal::line("[updater] boot partition set - restarting into the application now");
  esp_restart();
}

}  // namespace Updater
