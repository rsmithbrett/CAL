#include "Provisioning.h"

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

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
  uint8_t mac[6];
  WiFi.macAddress(mac);
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
    return "<p class=\"note\">No 2.4GHz networks found. This device cannot see "
           "5GHz networks.</p>";
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

void handleProbe() {
  // Mobile captive detection probes name external hosts, which wildcard DNS
  // resolves to us. An ABSOLUTE redirect avoids leaving captive.apple.com
  // (or another probe host) in the browser address bar.
  ++probeRequests;
  if (probeRequests <= 8) {
    Journal::printf("[portal] probe %.60s at +%lu ms", server.uri().c_str(),
                    static_cast<unsigned long>(millis() - portalStartedMs));
  }
  sendNoCacheHeaders();
  server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
  server.send(302, "text/html; charset=utf-8",
              "<!doctype html><title>WiFi setup</title>"
              "<p><a href=\"/\">Open device WiFi setup</a></p>");
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
  WiFi.mode(WIFI_AP_STA);
  const String name = apName();

  // OPEN, NOT WPA. The access point carried a MAC-derived passphrase until
  // 2026-10-04, and it cost the one thing this screen exists for: an iPhone has
  // to complete a WPA association before iOS raises the captive-portal sheet, so
  // the household watched a spinner before being shown anything to fill in.
  // Owner, from the bench: "it slowed the process of joining the iPhone."
  //
  // It bought very little. The passphrase was "dam" plus the last three bytes of
  // the MAC, and the SSID suffix already broadcasts two of them - guessable from
  // across the room by anyone who had seen one of these before.
  //
  // What an open portal exposes, stated rather than assumed: it serves a network
  // scan list and a form, it renders no stored credential, and the worst a
  // stranger could do is point this device at a network of their choosing. That
  // needs them inside WiFi range, during the few minutes the portal is up, of a
  // device its owner is standing over. A household that cannot join their own
  // display is the certain cost; that is the unlikely one.
  if (!WiFi.softAP(name.c_str())) {
    Journal::line("[portal] failed to start setup AP");
    Display::showFailure("Cannot open WiFi setup", "Restart the device.");
    return false;
  }

  const IPAddress ip = WiFi.softAPIP();
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
  // T:nopass AND NO P: FIELD, matching the open access point above. A payload
  // that still named WPA would have the phone attempt an encrypted association
  // against an open radio and fail to join at all - a worse outcome than the
  // delay this change set out to remove.
  const String joinPayload = "WIFI:S:" + name + ";T:nopass;;";
  Journal::printf("[portal] access point '%s' raised at %s - waiting up to %lu ms for "
                  "credentials",
                  name.c_str(), ip.toString().c_str(),
                  static_cast<unsigned long>(Config::kProvisioningAbandonTimeoutMs));
  // WiFi QR joins the temporary SSID only. iOS normally opens the captive
  // page; show the actual HTTP URL for phones that do not.
  const String setupUrl = "http://" + ip.toString();
  Journal::printf("[portal] manual browser URL: %s", setupUrl.c_str());
  Display::showQr(joinPayload, "Scan to join WiFi", name, "Open " + setupUrl);

  const uint32_t deadline = millis() + Config::kProvisioningAbandonTimeoutMs;
  bool abandoned = false;
  while (!credentialsAccepted) {
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
