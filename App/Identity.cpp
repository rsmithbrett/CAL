#include "Identity.h"

#include <Preferences.h>
#include <esp_mac.h>

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
constexpr const char* kKeyTotBoots = "totboots";
constexpr const char* kKeyUtcOffset = "utcoffmin";
constexpr const char* kKeySelfRestarts = "selfrst";

// Per-slot keys are built at runtime: "ssid0".."ssid2", "pass0".."pass2".
// NVS keys are capped at 15 characters, so these stay deliberately short.
String ssidKey(uint8_t i) { return "ssid" + String(i); }
String passKey(uint8_t i) { return "pass" + String(i); }

}  // namespace

void begin() {
  // Read-write. If this fails the partition is missing or corrupt, which is a
  // provisioning fault - CAL still runs, but with no identity it can only
  // report that.
  prefs.begin(kNamespace, false);
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

uint32_t totalBoots() { return prefs.getUInt(kKeyTotBoots, 0); }

void recordBoot() {
  const uint32_t current = totalBoots();
  // Saturate instead of wrapping - see Identity.h. A rolled-over 0 would look
  // like a device that had never booted, which is the one reading this counter
  // must never produce.
  if (current == UINT32_MAX) {
    return;
  }
  prefs.putUInt(kKeyTotBoots, current + 1);
}

int lastUtcOffsetMinutes() { return prefs.getInt(kKeyUtcOffset, 0); }

void setLastUtcOffsetMinutes(int minutes) { prefs.putInt(kKeyUtcOffset, minutes); }

uint8_t consecutiveSelfRestarts() { return prefs.getUChar(kKeySelfRestarts, 0); }

void recordSelfRestart() {
  const uint8_t current = consecutiveSelfRestarts();
  // Saturate instead of wrapping, same reasoning recordBoot() above states for
  // its own counter and for the same reason it matters more here: a wrap to 0
  // would hand a device that has restarted itself 256 times in a row the exact
  // reading of one that has never restarted at all, and buy it a fresh silence
  // budget on top. Pinned at 255, which is far past any threshold that reads
  // this.
  if (current == UINT8_MAX) {
    return;
  }
  prefs.putUChar(kKeySelfRestarts, current + 1);
}

void clearSelfRestarts() {
  // Guarded rather than written unconditionally. The caller is a once-per-boot
  // check in loop(), and the ordinary case by far is a device that never
  // restarted itself and has nothing to clear - writing a 0 over a 0 on every
  // boot would put a flash write into the steady state of every healthy device
  // in the fleet to record nothing at all.
  if (consecutiveSelfRestarts() == 0) {
    return;
  }
  prefs.putUChar(kKeySelfRestarts, 0);
}

}  // namespace Identity
