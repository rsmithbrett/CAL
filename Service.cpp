#include "Service.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <time.h>

#include "Config.h"
#include "Identity.h"
#include "Journal.h"
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
      Journal::printf("[time] SNTP settled at epoch %lu", static_cast<unsigned long>(now));
      return true;
    }
    delay(250);
  }

  // The distinction below is invisible from the screen, which says "cannot
  // reach the internet" either way, and the two want different first questions.
  // A clock still sitting at the epoch means no SNTP answer arrived at all; a
  // clock that moved but landed before kEarliestPlausibleTime means an answer
  // DID arrive and was rejected as implausible, which is a deliberate check
  // because SNTP is unauthenticated.
  const time_t finalTime = time(nullptr);
  Journal::printf("[time] SNTP failed after %lu ms - clock reads epoch %lu, needed > %lu",
                  static_cast<unsigned long>(Config::kSntpTimeoutMs),
                  static_cast<unsigned long>(finalTime),
                  static_cast<unsigned long>(Config::kEarliestPlausibleTime));
  return false;
}

Discovery fetchDiscovery() {
  Discovery out;

  // "Cannot reach the service" on screen has no visibility into which of
  // DNS/TLS/HTTP/heap actually failed, so each of them says so here instead.
  // This started life as TEMPORARY Serial instrumentation for the first
  // hardware test; it is now permanent and goes through the journal, so it
  // survives the boot that wrote it. See the README's "CAL can say why now".
  Journal::printf("[discovery] free heap before TLS: %u bytes",
                  static_cast<unsigned>(ESP.getFreeHeap()));

  NetworkClientSecure client;
  if (!Tls::configure(client)) {
    Journal::line("[discovery] Tls::configure failed - the embedded root bundle is empty, "
                  "so no request was attempted");
    return out;
  }

  HTTPClient http;
  const String url = String("https://") + Config::kServiceHost + Config::kWellKnownPath;
  Journal::printf("[discovery] GET %s", url.c_str());
  if (!http.begin(client, url)) {
    Journal::line("[discovery] http.begin() failed (malformed URL?)");
    return out;
  }

  http.setTimeout(Config::kHttpTimeoutMs);
  // Sent only when there is one. Discovery has to be reachable by a unit that
  // has not been assigned a key yet, since the enrollment endpoint it needs is
  // named in the document it returns. An empty header would read as a
  // malformed credential rather than as no credential.
  if (Identity::hasSecret()) {
    http.addHeader("X-Device-Secret", Identity::deviceSecret());
  } else {
    Journal::line("[discovery] no secret held, so no X-Device-Secret header was sent - "
                  "this is the normal unenrolled case, not an omission");
  }

  const int status = http.GET();
  Journal::printf("[discovery] HTTPClient status: %d, free heap after: %u bytes", status,
                  static_cast<unsigned>(ESP.getFreeHeap()));
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
    Journal::printf("[discovery] JSON parse failed: %s", err.c_str());
    return out;
  }

  out.manifestPath = doc["manifestPath"] | "/api/firmware/manifest";
  out.binaryPath = doc["binaryPath"] | "/api/firmware/current/binary";
  // NO DEFAULT, deliberately - see Discovery::calManifestPath. A server that does
  // not offer these is one that cannot update CAL, and a device inventing a path
  // would be guessing about the partition it cannot recover remotely.
  out.calManifestPath = doc["calManifestPath"] | "";
  out.calBinaryPath = doc["calBinaryPath"] | "";
  out.pairingPath = doc["pairingPath"] | "/api/enrollment/pairing-code";
  out.brandAssetPath = doc["brandAssetPath"] | "";
  out.enrollmentPath = doc["enrollmentPath"] | "/api/enrollment/bootstrap";
  out.qrUrl = doc["qrUrl"] | "";
  out.qrCaption = doc["qrCaption"] | "";
  out.httpTimeoutMs = doc["httpTimeoutMs"] | Config::kHttpTimeoutMs;
  out.ok = true;

  // Every path is recorded because they are all server-supplied and none of
  // them is compiled in - a later failure against one of these is otherwise
  // impossible to attribute to the document that named it.
  Journal::printf("[discovery] ok: manifest=%s binary=%s enrollment=%s brand=%s",
                  out.manifestPath.c_str(), out.binaryPath.c_str(),
                  out.enrollmentPath.c_str(),
                  out.brandAssetPath.length() > 0 ? out.brandAssetPath.c_str() : "(none)");
  Journal::printf("[discovery] qrUrl=%s httpTimeoutMs=%lu",
                  out.qrUrl.length() > 0 ? out.qrUrl.c_str() : "(none)",
                  static_cast<unsigned long>(out.httpTimeoutMs));
  return out;
}

}  // namespace Service
