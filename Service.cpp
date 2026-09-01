#include "Service.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <time.h>

#include "Config.h"
#include "Identity.h"
#include "Tls.h"

namespace Service {

bool synchroniseTime() {
  // Ordering is mandatory, not stylistic: join, then time, then TLS. See
  // Config::kEarliestPlausibleTime for why the result is range-checked.
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  const uint32_t deadline = millis() + Config::kSntpTimeoutMs;
  while (millis() < deadline) {
    const time_t now = time(nullptr);
    if (now > Config::kEarliestPlausibleTime) {
      return true;
    }
    delay(250);
  }
  return false;
}

Discovery fetchDiscovery() {
  Discovery out;

  NetworkClientSecure client;
  if (!Tls::configure(client)) {
    // No trust source means no safe request. The device presents its secret on
    // every call, so continuing unvalidated would hand that credential to
    // anything on the household network able to intercept.
    return out;
  }

  HTTPClient http;
  const String url = String("https://") + Config::kServiceHost + Config::kWellKnownPath;
  if (!http.begin(client, url)) {
    return out;
  }

  http.setTimeout(Config::kHttpTimeoutMs);
  // Sent only when there is one. Discovery has to be reachable by a unit that
  // has not been assigned a key yet, since the enrollment endpoint it needs is
  // named in the document it returns. An empty header would read as a
  // malformed credential rather than as no credential.
  if (Identity::hasSecret()) {
    http.addHeader("X-Device-Secret", Identity::deviceSecret());
  }

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

  out.manifestPath = doc["manifestPath"] | "/api/firmware/manifest";
  out.binaryPath = doc["binaryPath"] | "/api/firmware/current/binary";
  out.pairingPath = doc["pairingPath"] | "/api/enrollment/pairing-code";
  out.brandAssetPath = doc["brandAssetPath"] | "";
  out.enrollmentPath = doc["enrollmentPath"] | "/api/enrollment/bootstrap";
  out.qrUrl = doc["qrUrl"] | "";
  out.qrCaption = doc["qrCaption"] | "";
  out.httpTimeoutMs = doc["httpTimeoutMs"] | Config::kHttpTimeoutMs;
  out.ok = true;
  return out;
}

}  // namespace Service
