#include "WifiJoin.h"

#include <WiFi.h>

#include "Config.h"
#include "Display.h"
#include "Identity.h"
#include "Log.h"

// Join logic mirrors CAL's Provisioning::joinStoredNetwork() - scan, rank
// remembered networks by in-range signal, retry each a few times - because a
// device is enrolled in one place and used in another, so the most recently
// used network is not necessarily the one with usable signal here. Not shared
// as one file with CAL because CAL's version lives beside the captive-portal
// code it cooperates with (Identity::rememberNetwork on a fresh join); this
// copy has no such neighbor.
namespace WifiJoin {
namespace {

struct Candidate {
  uint8_t index;
  int32_t rssi;
};

bool attemptJoin(const Identity::Network& net) {
  for (uint8_t attempt = 1; attempt <= Config::kWifiJoinAttempts; ++attempt) {
    Display::showStatus("Connecting to WiFi",
                        net.ssid + "  (attempt " + String(attempt) + ")");
    WiFi.begin(net.ssid.c_str(), net.password.c_str());

    const uint32_t deadline = millis() + Config::kWifiJoinTimeoutMs;
    while (millis() < deadline) {
      if (WiFi.status() == WL_CONNECTED) {
        Log::printf("[wifi] joined SSID=%s IP=%s RSSI=%d dBm channel=%d", net.ssid.c_str(),
                    WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.channel());
        return true;
      }
      delay(250);
    }
    Log::printf("[wifi] attempt %d/%d for SSID=%s timed out", attempt, Config::kWifiJoinAttempts,
                net.ssid.c_str());
    WiFi.disconnect();
  }
  return false;
}

}  // namespace

bool joinStoredNetwork() {
  const uint8_t known = Identity::networkCount();
  if (known == 0) {
    Log::line("[wifi] no networks remembered, cannot join");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setMinSecurity(WIFI_AUTH_WEP);

  Display::showStatus("Looking for known networks", "");
  const int found = WiFi.scanNetworks();

  Candidate candidates[Identity::kMaxNetworks];
  uint8_t count = 0;

  for (uint8_t i = 0; i < known; ++i) {
    const Identity::Network net = Identity::network(i);
    if (net.ssid.length() == 0) {
      continue;
    }
    int32_t best = INT32_MIN;
    for (int s = 0; s < found; ++s) {
      if (WiFi.SSID(s) == net.ssid && WiFi.RSSI(s) > best) {
        best = WiFi.RSSI(s);
      }
    }
    if (best != INT32_MIN) {
      candidates[count].index = i;
      candidates[count].rssi = best;
      ++count;
    }
  }
  WiFi.scanDelete();
  Log::printf("[wifi] scan found %d networks, %d match a remembered SSID in range", found, count);

  for (uint8_t i = 1; i < count; ++i) {
    const Candidate key = candidates[i];
    int8_t j = static_cast<int8_t>(i) - 1;
    while (j >= 0 && candidates[j].rssi < key.rssi) {
      candidates[j + 1] = candidates[j];
      --j;
    }
    candidates[j + 1] = key;
  }

  for (uint8_t i = 0; i < count; ++i) {
    const Identity::Network net = Identity::network(candidates[i].index);
    if (attemptJoin(net)) {
      Identity::rememberNetwork(net.ssid, net.password);
      return true;
    }
  }

  if (count == 0) {
    Log::line("[wifi] none of the remembered networks turned up in the scan, trying them blind");
    for (uint8_t i = 0; i < known; ++i) {
      const Identity::Network net = Identity::network(i);
      if (net.ssid.length() > 0 && attemptJoin(net)) {
        Identity::rememberNetwork(net.ssid, net.password);
        return true;
      }
    }
  }

  Log::line("[wifi] exhausted all remembered networks, none reachable");
  return false;
}

}  // namespace WifiJoin
