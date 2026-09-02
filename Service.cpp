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

  // TEMPORARY diagnostic instrumentation for the first real-hardware test -
  // "Cannot reach the service" on screen has no visibility into which of
  // DNS/TLS/HTTP/heap actually failed. Remove once a device has completed
  // this call successfully at least once.
  Serial.printf("[discovery] free heap before TLS: %u bytes\n", ESP.getFreeHeap());

  NetworkClientSecure client;
  if (!Tls::configure(client)) {
    Serial.println("[discovery] Tls::configure failed - empty cert bundle");
    return out;
  }

  HTTPClient http;
  const String url = String("https://") + Config::kServiceHost + Config::kWellKnownPath;
  Serial.printf("[discovery] GET %s\n", url.c_str());
  if (!http.begin(client, url)) {
    Serial.println("[discovery] http.begin() failed (malformed URL?)");
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
  Serial.printf("[discovery] HTTPClient status: %d, free heap after: %u bytes\n",
                status, ESP.getFreeHeap());
  if (status != 200) {
    // Negative values here are HTTPClient's own error codes (connection
    // refused, TLS failure, DNS failure, timeout) - see HTTPClient.h's
    // HTTPC_ERROR_* constants for what each number means.
    http.end();
    return out;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.printf("[discovery] JSON parse failed: %s\n", err.c_str());
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
