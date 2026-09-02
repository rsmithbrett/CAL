#include "AppUpdater.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>

#include "Config.h"
#include "Identity.h"
#include "Tls.h"

namespace AppUpdater {

bool newerVersionAvailable() {
  NetworkClientSecure client;
  if (!Tls::configure(client)) {
    return false;
  }

  HTTPClient http;
  const String url = String("https://") + Config::kServiceHost + Config::kManifestPath;
  if (!http.begin(client, url)) {
    return false;
  }
  http.setTimeout(Config::kHttpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());

  const int status = http.GET();
  if (status != 200) {
    http.end();
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    return false;
  }

  const bool isConfigured = doc["isConfigured"] | false;
  const String version = doc["version"] | "";
  // An empty version answers "no build is configured" identically to
  // isConfigured being false - checked separately anyway, since a
  // manifest that is misconfigured to report isConfigured=true with no
  // version must not be read as "always different, always update".
  return isConfigured && version.length() > 0 && version != Identity::installedAppVersion();
}

}  // namespace AppUpdater
