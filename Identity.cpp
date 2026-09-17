#include "Identity.h"

#include <Preferences.h>
#include <esp_mac.h>

#include "Journal.h"

namespace Identity {
namespace {

Preferences prefs;

// One namespace for everything CAL persists. Kept short: NVS keys are limited
// to 15 characters and silently truncate past that.
constexpr const char* kNamespace = "cal";

constexpr const char* kKeySecret = "secret";
constexpr const char* kKeyNetCount = "netcount";
constexpr const char* kKeyAppVer = "appver";
constexpr const char* kKeyUpdReq = "updreq";
constexpr const char* kKeyBootAtt = "bootatt";
constexpr const char* kKeyProvForced = "provforced";
// Phase 2 of the CAL-over-the-air design. A candidate CAL sitting in ota_0
// cannot measure its own length - the partition is larger than the image - so
// phase 1 records what it downloaded and the candidate is told. See
// CAL_OTA_DESIGN.md section 10.
constexpr const char* kKeyCalSize = "calsize";
constexpr const char* kKeyCalSha = "calsha";
constexpr const char* kKeyCalClean = "calclean";
constexpr const char* kKeyCalTries = "caltries";
// Which CAL is installed in factory, written by the trampoline when it commits -
// the exact mirror of kKeyAppVer, which installApplication writes for the App.
// The App reads this back and puts it on telemetry, because CAL does not check in.
constexpr const char* kKeyCalVer = "calver";
// The version string of the candidate sitting in ota_0, recorded by phase 1 beside
// its size and hash. Becomes kKeyCalVer once the trampoline commits it.
constexpr const char* kKeyCalCandVer = "calcandver";

// Per-slot keys are built at runtime: "ssid0".."ssid2", "pass0".."pass2".
// NVS keys are capped at 15 characters, so these stay deliberately short.
String ssidKey(uint8_t i) { return "ssid" + String(i); }
String passKey(uint8_t i) { return "pass" + String(i); }

}  // namespace

void begin() {
  // Read-write. If this fails the partition is missing or corrupt, which is a
  // provisioning fault - CAL still runs, but with no identity it can only
  // report that.
  //
  // The return value used to be discarded. It is the difference between "this
  // device has never been given a secret" and "this device cannot read the
  // partition its secret is in", and every symptom downstream - enrollment
  // looping, the manifest fetch being refused - looks identical either way.
  if (!prefs.begin(kNamespace, false)) {
    Journal::line("[identity] NVS namespace 'cal' would not open read-write - the nvs "
                  "partition is missing or corrupt. Every value below reads as its "
                  "default, which is NOT the same as the device never having been "
                  "provisioned.");
  }
}

String deviceSecret() { return prefs.getString(kKeySecret, ""); }

bool hasSecret() { return deviceSecret().length() > 0; }

void saveSecret(const String& secret) { prefs.putString(kKeySecret, secret); }

void clearSecret() { prefs.remove(kKeySecret); }

String macAddress() {
  uint8_t mac[6];
  // The station address specifically. A device reporting its soft-AP address
  // during provisioning and its station address afterwards would look like two
  // different units to the server.
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char text[18];
  snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
  return String(text);
}

uint8_t networkCount() {
  const uint8_t stored = prefs.getUChar(kKeyNetCount, 0);
  return stored > kMaxNetworks ? kMaxNetworks : stored;
}

Network network(uint8_t index) {
  Network n;
  if (index >= networkCount()) {
    return n;
  }
  n.ssid = prefs.getString(ssidKey(index).c_str(), "");
  n.password = prefs.getString(passKey(index).c_str(), "");
  return n;
}

void rememberNetwork(const String& ssid, const String& password) {
  if (ssid.length() == 0) {
    return;
  }

  // Read the existing list out, drop any entry for this SSID, and rebuild with
  // the new one at the front. Rewriting all three slots is cheap at this size
  // and avoids the shuffling bugs an in-place move invites.
  Network existing[kMaxNetworks];
  const uint8_t had = networkCount();
  for (uint8_t i = 0; i < had; ++i) {
    existing[i] = network(i);
  }

  uint8_t written = 0;
  prefs.putString(ssidKey(0).c_str(), ssid);
  prefs.putString(passKey(0).c_str(), password);
  written = 1;

  for (uint8_t i = 0; i < had && written < kMaxNetworks; ++i) {
    if (existing[i].ssid == ssid || existing[i].ssid.length() == 0) {
      continue;  // the entry just promoted to the front
    }
    prefs.putString(ssidKey(written).c_str(), existing[i].ssid);
    prefs.putString(passKey(written).c_str(), existing[i].password);
    ++written;
  }

  // Anything past the new count is left in NVS but is unreachable, since every
  // read is bounded by networkCount().
  prefs.putUChar(kKeyNetCount, written);
}

void clearNetworks() {
  for (uint8_t i = 0; i < kMaxNetworks; ++i) {
    prefs.remove(ssidKey(i).c_str());
    prefs.remove(passKey(i).c_str());
  }
  prefs.putUChar(kKeyNetCount, 0);
}

bool hasAnyNetwork() { return networkCount() > 0; }

bool provisioningForced() { return prefs.getBool(kKeyProvForced, false); }

void setProvisioningForced(bool forced) { prefs.putBool(kKeyProvForced, forced); }

String installedAppVersion() { return prefs.getString(kKeyAppVer, ""); }

void setInstalledAppVersion(const String& version) {
  prefs.putString(kKeyAppVer, version);
}

bool updateRequested() { return prefs.getBool(kKeyUpdReq, false); }

void setUpdateRequested(bool requested) { prefs.putBool(kKeyUpdReq, requested); }

uint8_t bootAttempts() { return prefs.getUChar(kKeyBootAtt, 0); }

void recordBootAttempt() {
  // Written before control is handed to the application, and only cleared once
  // the application reports itself healthy. An application that crashes during
  // startup therefore increments this on every attempt and is eventually
  // recognised as bad rather than being retried forever.
  prefs.putUChar(kKeyBootAtt, bootAttempts() + 1);
}

void clearBootAttempts() { prefs.putUChar(kKeyBootAtt, 0); }

// --- Phase 2 candidate state (CAL_OTA_DESIGN.md section 10) ------------------

void recordCalCandidate(uint32_t sizeBytes, const String& sha256, const String& version) {
  prefs.putString(kKeyCalCandVer, version);
  prefs.putULong(kKeyCalSize, sizeBytes);
  prefs.putString(kKeyCalSha, sha256);
  prefs.putUChar(kKeyCalTries, 0);
}

uint32_t calCandidateSize() { return prefs.getULong(kKeyCalSize, 0); }
String calCandidateSha() { return prefs.getString(kKeyCalSha, ""); }
String calCandidateVersion() { return prefs.getString(kKeyCalCandVer, ""); }

void clearCalCandidate() {
  prefs.remove(kKeyCalSize);
  prefs.remove(kKeyCalSha);
  prefs.remove(kKeyCalCandVer);
  prefs.remove(kKeyCalTries);
}

// Set by the candidate immediately before it erases otadata, and acted on by the
// NEXT boot - the new CAL running from factory. The candidate cannot erase ota_0
// itself because it is executing from it. This marker is what stops a boot loop:
// a factory CAL that found a valid CAL image in ota_0 would hand over to it, and
// that candidate would copy and reboot forever.
void setCalCleanupPending(bool pending) { prefs.putUChar(kKeyCalClean, pending ? 1 : 0); }
bool calCleanupPending() { return prefs.getUChar(kKeyCalClean, 0) != 0; }

uint8_t calCopyAttempts() { return prefs.getUChar(kKeyCalTries, 0); }
void recordCalCopyAttempt() { prefs.putUChar(kKeyCalTries, calCopyAttempts() + 1); }

String installedCalVersion() { return prefs.getString(kKeyCalVer, ""); }
void setInstalledCalVersion(const String& version) { prefs.putString(kKeyCalVer, version); }

}  // namespace Identity
