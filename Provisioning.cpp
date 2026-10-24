#include "Provisioning.h"

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_mac.h>

#include "BootButton.h"
#include "Config.h"
#include "Display.h"
#include "Identity.h"
#include "Journal.h"

namespace Provisioning {
namespace {

DNSServer dns;
WebServer server(80);
bool credentialsAccepted = false;
String cachedNetworkOptionsHtml;
uint32_t portalStartedMs = 0;
uint16_t probeRequests = 0;

void sendNoCacheHeaders() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
}

String apName() {
  // The MAC suffix matters: two devices in one household otherwise advertise
  // identical names and the phone silently joins whichever it saw first.
  //
  // esp_read_mac, NOT WiFi.macAddress(). The latter reads the STATION interface,
  // and this portal runs AP-only as of 2026-10-24, so the station is down and it
  // answers all zeros: every unit advertised "Discover-Setup-0000" on the first
  // build after that change, which is exactly the collision the line above warns
  // about. esp_read_mac reads the burned-in address and does not care which
  // interfaces are up - it is what Identity::macAddress has always used, which is
  // why the journal kept printing the right address while the SSID did not.
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%02X%02X", mac[4], mac[5]);
  return String(Config::kSetupApPrefix) + "-" + suffix;
}


String scanNetworksHtml() {
  // The portal offers what the device itself scanned rather than a free-text
  // field. The device sees only 2.4GHz; a band-steering router advertises one
  // name across both, so a network plainly visible on the phone can be
  // genuinely invisible here. Showing the device's own view makes that legible
  // instead of presenting as a wrong password on a correct one.
  const int found = WiFi.scanNetworks();
  String html;
  if (found <= 0) {
    WiFi.scanDelete();
    return "<p class=\"note\">No 2.4GHz networks found during setup scan. "
           "Enter the network name below.</p>";
  }
  // Bound the HTML stored while the AP is active; the phone can enter an
  // SSID manually when a network is missing or hidden.
  const int limit = found < 24 ? found : 24;
  html.reserve(limit * 100);
  html += "<select name=\"ssid\" id=\"ssid\">";
  for (int i = 0; i < limit; ++i) {
    // SSIDs are supplied by nearby radios. Escape them in both the visible
    // option text and its HTML value attribute.
    String label = WiFi.SSID(i);
    label.replace("&", "&amp;");
    label.replace("<", "&lt;");
    label.replace(">", "&gt;");
    label.replace("\"", "&quot;");
    label.replace("'", "&#39;");
    html += "<option value=\"" + label + "\">" + label + " (" +
            String(WiFi.RSSI(i)) + "dBm)</option>";
  }
  html += "</select>";
  WiFi.scanDelete();
  return html;
}

void handleRoot() {
  const uint32_t started = millis();
  Journal::printf("[portal] root request at +%lu ms",
                  static_cast<unsigned long>(started - portalStartedMs));
  String page =
      "<!doctype html><meta name=viewport content=\"width=device-width,initial-scale=1\">"
      "<style>body{font-family:system-ui;margin:0;padding:24px;background:#111;color:#eee}"
      "h1{font-size:20px;font-weight:500}label{display:block;margin:16px 0 6px;font-size:14px}"
      "select,input{width:100%;padding:10px;font-size:16px;border-radius:8px;border:1px solid #444;"
      "background:#1c1c1c;color:#eee;box-sizing:border-box}"
      "button{margin-top:20px;width:100%;padding:12px;font-size:16px;border:0;border-radius:8px;"
      "background:#2a78d6;color:#fff}.note{color:#9a9a9a;font-size:13px}</style>"
      "<h1>Connect this device to WiFi</h1>"
      "<form method=POST action=/save><label for=ssid>Network</label>";
  // This was prepared before WiFi.softAP(): no radio scanning during HTTP.
  page += cachedNetworkOptionsHtml;
  page +=
      "<label for=manualSsid>Or enter a missing / hidden network</label>"
      "<input id=manualSsid name=manualSsid maxlength=32 autocomplete=off>"
      "<label for=pass>Password</label><input id=pass name=pass type=password>"
      "<button type=submit>Connect</button></form>"
      "<p class=\"note\">Only 2.4GHz networks are supported. Typing a network name "
      "overrides the selection above.</p>";
  sendNoCacheHeaders();
  server.send(200, "text/html; charset=utf-8", page);
  Journal::printf("[portal] root served in %lu ms",
                  static_cast<unsigned long>(millis() - started));
}

void handleSave() {
  const String manualSsid = server.arg("manualSsid");
  const String ssid = manualSsid.length() ? manualSsid : server.arg("ssid");
  const String pass = server.arg("pass");
  sendNoCacheHeaders();
  if (ssid.length() == 0 || ssid.length() > 32 || pass.length() > 64) {
    server.send(400, "text/plain", "A valid network name and password are required.");
    return;
  }
  Identity::rememberNetwork(ssid, pass);
  credentialsAccepted = true;
  // The SSID, never the passphrase. The journal is read off the flash of a
  // device that may not belong to the person reading it.
  Journal::printf("[portal] credentials accepted for SSID '%s'", ssid.c_str());
  server.send(200, "text/html",
              "<!doctype html><meta name=viewport content=\"width=device-width,initial-scale=1\">"
              "<body style=\"font-family:system-ui;background:#111;color:#eee;padding:24px\">"
              "<h1 style=\"font-size:20px;font-weight:500\">Connecting</h1>"
              "<p>You can close this page and look at the device.</p>");
}

// The reason codes a setup AP actually sees, named. The full wifi_err_reason_t has
// about fifty entries and compiling all of them in would cost flash to say nothing:
// these are the ones that separate "wrong passphrase" from "the phone went away".
//
// 15 is the one to look for. A four-way handshake timeout is what a handset offering
// a stale passphrase produces, and the wired install erases NVS, so the key changes
// on every reflash while the SSID does not.
const char* disconnectReason(uint8_t reason) {
  switch (reason) {
    case 2:   return "previous authentication no longer valid";
    case 3:   return "the station left";
    case 4:   return "inactive too long";
    case 8:   return "the station left the network";
    case 15:  return "four-way handshake timed out - usually a wrong passphrase";
    case 23:  return "802.1X authentication failed";
    case 204: return "handshake timeout";
    default:  return "see wifi_err_reason_t";
  }
}

void handleProbe() {
  // Mobile platforms probe a known URL to decide whether a network has
  // internet. Answered wrongly, the handset declares the network dead and
  // silently drops back to cellular partway through setup, taking the form
  // with it. Wildcard DNS alone does not prevent that.
  //
  // THE REDIRECT IS RELATIVE, AND HAS TO BE. v2026.10.23.0001 made it
  // absolute - "http://192.168.4.1/" - to keep captive.apple.com out of the
  // address bar, and the portal stopped appearing: an iPhone joined the setup
  // network and then sat there with no form, which is how it was found.
  //
  // Wildcard DNS has already pointed the probe host at this device, so "/"
  // keeps the handset on the host it asked for and the captive sheet follows
  // it. An absolute redirect sends it to a different host mid-probe, which
  // iOS reads as leaving the captive context and answers by closing the sheet
  // rather than following it. A tidier address bar is not worth a setup
  // nobody can finish.
  ++probeRequests;
  if (probeRequests <= 8) {
    Journal::printf("[portal] probe %.60s at +%lu ms", server.uri().c_str(),
                    static_cast<unsigned long>(millis() - portalStartedMs));
  }
  sendNoCacheHeaders();
  server.sendHeader("Location", "/", true);
  // A BODY, NOT AN EMPTY ONE. This redirect carried a zero-length body until
  // 2026-10-24. Espressif's own CaptivePortal example sends text with its 302,
  // and an iPad that received seven of these, each answered in 3 ms, still never
  // put the form on screen. A body costs nothing and removes one difference
  // between this portal and the reference one.
  server.send(302, "text/html; charset=utf-8",
              "<!doctype html><html><body>"
              "Opening DiscoverAroundMe WiFi setup..."
              "</body></html>");
}

}  // namespace

namespace {

/// A remembered network that is actually in range, with the signal we saw.
struct Candidate {
  uint8_t index;
  int32_t rssi;
};

/// Set by attemptJoin() when the household holds BOOT during the ladder. Read
/// and cleared by joinStoredNetwork(), which stops laddering and lets the
/// caller open the portal.
bool gSetupRequestedDuringJoin = false;

/// How long BOOT must be held during the ladder. The same three seconds as the
/// power-on gesture, so there is one hold time to learn.
constexpr uint32_t kSetupHoldMs = 3000;

bool attemptJoin(const Identity::Network& net) {
  for (uint8_t attempt = 1; attempt <= Config::kWifiJoinAttempts; ++attempt) {
    Display::showStatus("Connecting to WiFi",
                        net.ssid + "  (attempt " + String(attempt) + ")");
    WiFi.begin(net.ssid.c_str(), net.password.c_str());

    const uint32_t deadline = millis() + Config::kWifiJoinTimeoutMs;
    bool prompted = false;
    while (millis() < deadline) {
      if (WiFi.status() == WL_CONNECTED) {
        Journal::printf("[wifi] joined '%s' on attempt %u of %u", net.ssid.c_str(),
                        static_cast<unsigned>(attempt),
                        static_cast<unsigned>(Config::kWifiJoinAttempts));
        return true;
      }

      // The household is standing in front of the unit while this runs, and
      // until now nothing watched the button until the next power-on.
      if (BootButton::isDown()) {
        if (!prompted) {
          Display::showStatus("Keep holding BOOT to set up WiFi", "Release now to cancel");
          prompted = true;
        }
        if (BootButton::heldFor(kSetupHoldMs)) {
          Journal::line("[wifi] BOOT held during the join ladder - stopping and opening "
                        "WiFi setup");
          BootButton::reset();
          gSetupRequestedDuringJoin = true;
          WiFi.disconnect();
          return false;
        }
      } else if (prompted) {
        // Released early. Put the attempt back on the glass so the screen does
        // not keep telling somebody to hold a button they let go of.
        Display::showStatus("Connecting to WiFi",
                            net.ssid + "  (attempt " + String(attempt) + ")");
        prompted = false;
      }

      delay(250);
    }
    // WiFi.status() at the moment of the timeout separates a wrong passphrase
    // (WL_CONNECT_FAILED) from an AP that never answered (WL_NO_SSID_AVAIL or
    // still WL_DISCONNECTED), which the screen cannot show and the household
    // cannot tell apart.
    Journal::printf("[wifi] attempt %u of %u on '%s' timed out after %lu ms, status=%d",
                    static_cast<unsigned>(attempt),
                    static_cast<unsigned>(Config::kWifiJoinAttempts), net.ssid.c_str(),
                    static_cast<unsigned long>(Config::kWifiJoinTimeoutMs),
                    static_cast<int>(WiFi.status()));
    WiFi.disconnect();
  }
  return false;
}

}  // namespace

bool setupRequestedDuringJoin() {
  const bool requested = gSetupRequestedDuringJoin;
  gSetupRequestedDuringJoin = false;
  return requested;
}

bool joinStoredNetwork() {
  gSetupRequestedDuringJoin = false;
  BootButton::begin();

  const uint8_t known = Identity::networkCount();
  if (known == 0) {
    Journal::line("[wifi] nothing remembered - no join attempted, going straight to the "
                  "setup portal");
    return false;
  }

  WiFi.mode(WIFI_STA);
  // arduino-esp32 3.x raised the default minimum security threshold and can
  // refuse real household networks without this.
  WiFi.setMinSecurity(WIFI_AUTH_WEP);

  // Scan before choosing. A device is enrolled in one place and used in
  // another, so more than one remembered network may be in range - and the
  // most recently used is not necessarily the one with usable signal.
  Display::showStatus("Looking for known networks", "");
  const int found = WiFi.scanNetworks();
  Journal::printf("[wifi] scan saw %d networks; %u are remembered", found,
                  static_cast<unsigned>(known));

  Candidate candidates[Identity::kMaxNetworks];
  uint8_t count = 0;

  for (uint8_t i = 0; i < known; ++i) {
    const Identity::Network net = Identity::network(i);
    if (net.ssid.length() == 0) {
      Journal::printf("[wifi] remembered slot %u is empty - skipped",
                      static_cast<unsigned>(i));
      continue;
    }
    int32_t best = INT32_MIN;
    for (int s = 0; s < found; ++s) {
      // The same name can appear more than once on a mesh; take the strongest.
      if (WiFi.SSID(s) == net.ssid && WiFi.RSSI(s) > best) {
        best = WiFi.RSSI(s);
      }
    }
    if (best != INT32_MIN) {
      Journal::printf("[wifi] remembered '%s' is in range at %d dBm", net.ssid.c_str(),
                      static_cast<int>(best));
      candidates[count].index = i;
      candidates[count].rssi = best;
      ++count;
    } else {
      // Worth saying out loud: this device sees 2.4GHz only, so a network the
      // household can plainly see on a phone can genuinely be invisible here.
      Journal::printf("[wifi] remembered '%s' was NOT in the scan (2.4GHz only)",
                      net.ssid.c_str());
    }
  }
  WiFi.scanDelete();

  // Strongest first. Insertion sort over at most three entries.
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
      // Promotes it to most-recently-used, so the list reflects where the
      // device actually lives rather than where it was last provisioned.
      Identity::rememberNetwork(net.ssid, net.password);
      return true;
    }
    // A hold means the household has told us the network changed. Trying the
    // rest of the list is the wait they just asked to end.
    if (gSetupRequestedDuringJoin) {
      return false;
    }
  }

  // Nothing remembered was in range. Falling back to trying them blind covers
  // a network that is present but was missed by the scan, which happens.
  if (count == 0) {
    Journal::line("[wifi] no remembered network appeared in the scan - trying all of "
                  "them blind, in case the scan missed one");
    for (uint8_t i = 0; i < known; ++i) {
      const Identity::Network net = Identity::network(i);
      if (net.ssid.length() > 0 && attemptJoin(net)) {
        Identity::rememberNetwork(net.ssid, net.password);
        return true;
      }
      if (gSetupRequestedDuringJoin) {
        return false;
      }
    }
  } else {
    // Named explicitly so the journal distinguishes "we never tried the blind
    // fallback" from "we tried it and it also failed".
    Journal::printf("[wifi] %u in-range candidate(s) all failed to join - the blind "
                    "fallback was NOT attempted, it only runs when the scan found none",
                    static_cast<unsigned>(count));
  }

  Journal::line("[wifi] could not join any remembered network");
  return false;
}

bool run() {
  credentialsAccepted = false;
  probeRequests = 0;

  // AP+STA is one radio: a synchronous scan during handleRoot() delayed the
  // iPhone portal and can interrupt its captive detection connection.
  // Scan first in STA mode, then start the AP and never scan it again.
  WiFi.mode(WIFI_STA);
  Display::showStatus("Finding WiFi networks", "Preparing setup");
  const uint32_t scanStarted = millis();
  cachedNetworkOptionsHtml = scanNetworksHtml();
  Journal::printf("[portal] pre-AP scan done in %lu ms; HTML=%u bytes",
                  static_cast<unsigned long>(millis() - scanStarted),
                  static_cast<unsigned>(cachedNetworkOptionsHtml.length()));
  // THE STATION STAYS REGISTERED AND STOPS TRYING. Both halves of that matter.
  //
  // It used to retry a remembered network for as long as the portal was up, on the
  // one radio the access point needs. When that network is out of range or 5GHz the
  // retry never succeeds and never stops, and the access point degrades as the
  // session runs: measured 2026-10-24 at 189 seconds of stable association on a
  // clean device, then 105, then under four.
  //
  // The first attempt at this dropped to WIFI_AP outright, and that was worse. It
  // tears the station interface down, and the switch back to WIFI_STA afterwards
  // failed to re-register the network stack - "netstack cb reg failed with 12308" -
  // so the join that follows could not succeed at all. A household network at -5 dBm
  // with the right password timed out with status 4. Keeping AP_STA keeps the
  // interface; disconnect plus setAutoReconnect(false) is what stops the retrying,
  // and it is the whole of what was wrong.
  //
  // joinStoredNetwork calls WiFi.begin(), which re-arms auto-reconnect itself.
  //
  // Left as AP_STA it does active harm. Once a network is remembered - which it is
  // from the moment somebody submits the form once - the station side retries that
  // association for as long as the portal is up. If the remembered network is out of
  // range or 5GHz, which is the case this was found on, it never succeeds and never
  // stops, and the access point degrades as the session runs: measured 2026-10-24 at
  // 189 seconds of stable association on a clean device, then 105, then under four.
  //
  // WiFi.disconnect() first so a connection attempt already in flight from the join
  // ladder is dropped rather than left running underneath the mode change. false,
  // false keeps the radio on and keeps the stored credentials, which the ladder needs
  // again the moment this returns.
  WiFi.disconnect(false, false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_AP_STA);
  const String name = apName();

  // WPA2 bench experiment. iPhone Camera reads the passphrase from the Wi-Fi
  // QR automatically, so the person still types no *setup AP* password.
  // Preserve the same SSID/password across power cycles to avoid stale phone
  // credentials. If NVS cannot retain the key, do not start an open portal.
  // WHY A HANDSHAKE FAILED, NOT JUST THAT ONE DID. The station count says a phone
  // arrived or left and nothing about why. The AP events carry a reason code, which
  // separates a wrong passphrase from a phone that wandered off - and a wrong
  // passphrase is the expected failure here, because the wired install runs
  // erase_flash before write_flash. That wipes NVS, so every reflash mints a NEW key
  // for the SAME SSID while the handset keeps offering the old one.
  //
  // Registered before softAP() so nothing is missed between the radio coming up and
  // the first association. Logs the MAC's last three bytes only: enough to follow one
  // handset across a session, and not the whole address of somebody's phone.
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    const uint8_t* m = info.wifi_ap_staconnected.mac;
    Journal::printf("[portal] station %02X:%02X:%02X associated", m[3], m[4], m[5]);
  }, ARDUINO_EVENT_WIFI_AP_STACONNECTED);

  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    const uint8_t* m = info.wifi_ap_stadisconnected.mac;
    const uint8_t why = info.wifi_ap_stadisconnected.reason;
    Journal::printf("[portal] station %02X:%02X:%02X left, reason=%u (%s)",
                    m[3], m[4], m[5], static_cast<unsigned>(why), disconnectReason(why));
  }, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);

  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    const esp_ip4_addr_t ip = info.wifi_ap_staipassigned.ip;
    Journal::printf("[portal] lease issued: %u.%u.%u.%u",
                    ip.addr & 0xFF, (ip.addr >> 8) & 0xFF,
                    (ip.addr >> 16) & 0xFF, (ip.addr >> 24) & 0xFF);
  }, ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED);

  const String setupPassword = Identity::setupApPassword();
  if (setupPassword.length() != 24) {
    Journal::line("[portal] protected setup AP key unavailable - refusing open AP");
    Display::showFailure("Cannot open WiFi setup", "Restart or recover over USB.");
    return false;
  }

  if (!WiFi.softAP(name.c_str(), setupPassword.c_str())) {
    Journal::line("[portal] failed to start setup AP");
    Display::showFailure("Cannot open WiFi setup", "Restart the device.");
    return false;
  }

  const IPAddress ip = WiFi.softAPIP();

  // DHCP OPTION 114, RFC 8910 - the thing a hotel does that this did not.
  // Handed to the phone in its DHCP lease, it names the portal outright instead
  // of leaving iOS to infer one from probe answers. The core builds the URI as
  // "http://" plus the softAP address, so it is this device and nothing else.
  //
  // NOT standards-complete, and that is stated rather than hidden: RFC 8910's
  // option points at an RFC 8908 API endpoint, which must be HTTPS serving
  // application/captive+json. A device on 192.168.4.1 cannot hold a certificate
  // for a name anybody trusts, so this advertises a plain HTTP page and a client
  // is free to ignore it. It is additive: the wildcard DNS and the probe handlers
  // below are unchanged and still carry the phones that do.
  //
  // THE VERSION GUARD IS LOGGED, NOT SILENT. enableDhcpCaptivePortal() exists
  // only from ESP-IDF 5.4.2, and CAL is built against two different trees - CI
  // builds stock, this workstation carries an overlay. Compiled out with no line
  // in the journal, a binary that never had the call would read exactly like one
  // where the option did not help.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 2)
  const bool advertised = WiFi.AP.enableDhcpCaptivePortal();
  Journal::printf("[portal] DHCP option 114 advertising http://%s: %s",
                  ip.toString().c_str(), advertised ? "accepted" : "refused by the core");
#else
  Journal::line("[portal] DHCP option 114 not compiled in - this core predates ESP-IDF 5.4.2");
#endif

  dns.start(53, "*", ip);

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/generate_204", handleProbe);          // Android
  server.on("/hotspot-detect.html", handleProbe);   // iOS / macOS
  server.on("/connecttest.txt", handleProbe);       // Windows
  server.onNotFound(handleProbe);
  server.begin();
  portalStartedMs = millis();

  // The code carries the device's own access point in the format phone cameras
  // already understand, so scanning it joins the phone to the device. This
  // removes the step that fails most often: hunting for an unfamiliar network
  // name in a list.
  //
  // WPA2 passphrase travels in the QR. Never include it in the boot journal.
  // Uppercase hex needs no escaping in Wi-Fi QR grammar.
  const String joinPayload = "WIFI:T:WPA;S:" + name +
                             ";P:" + setupPassword + ";;";
  Journal::printf("[portal] WPA2 access point '%s' raised at %s - waiting up to %lu ms for "
                  "credentials",
                  name.c_str(), ip.toString().c_str(),
                  static_cast<unsigned long>(Config::kProvisioningAbandonTimeoutMs));
  // WiFi QR joins the temporary SSID only. iOS normally opens the captive
  // page; show the actual HTTP URL for phones that do not.
  const String setupUrl = String("http://") + ip.toString();
  Journal::printf("[portal] manual browser URL: %s", setupUrl.c_str());
  Display::showQr(joinPayload, "Scan to join WiFi", name, String("Open ") + setupUrl);

  const uint32_t deadline = millis() + Config::kProvisioningAbandonTimeoutMs;
  bool abandoned = false;
  int lastStationCount = 0;
  while (!credentialsAccepted) {
    // iOS may associate, fetch the captive page and never bring CNA onscreen.
    // Association timing gives a firm baseline for comparing Camera/Settings.
    const int stationCount = WiFi.softAPgetStationNum();
    if (stationCount != lastStationCount) {
      Journal::printf("[portal] station count=%d at +%lu ms",
                      stationCount,
                      static_cast<unsigned long>(millis() - portalStartedMs));
      lastStationCount = stationCount;
    }
    if (millis() >= deadline) {
      abandoned = true;
      break;
    }

    dns.processNextRequest();
    server.handleClient();
    delay(2);
  }

  if (!abandoned) {
    delay(400);  // let the acknowledgement page reach the handset
  } else {
    Journal::printf("[portal] abandoned - nobody submitted credentials within %lu ms",
                    static_cast<unsigned long>(Config::kProvisioningAbandonTimeoutMs));
  }
  server.stop();
  dns.stop();
  WiFi.softAPdisconnect(true);
  cachedNetworkOptionsHtml = "";
  Journal::printf("[portal] closed; probes=%u, returning %s",
                  static_cast<unsigned>(probeRequests), abandoned ? "false" : "true");
  return !abandoned;
}

}  // namespace Provisioning
