#include "AppUpdater.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>

#include "Config.h"
#include "Identity.h"
#include "Log.h"
#include "Tls.h"

namespace AppUpdater {

bool newerVersionAvailable() {
  NetworkClientSecure client;
  if (!Tls::configure(client)) {
    Log::line("[update] TLS setup failed while checking the manifest");
    return false;
  }

  HTTPClient http;
  const String url = String("https://") + Config::kServiceHost + Config::kManifestPath;
  if (!http.begin(client, url)) {
    Log::line("[update] could not begin manifest request");
    return false;
  }
  http.setTimeout(Config::kHttpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());

  const int status = http.GET();
  if (status != 200) {
    http.end();
    Log::printf("[update] manifest check failed, http status=%d", status);
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Log::line("[update] manifest response was not valid JSON");
    return false;
  }

  const bool isConfigured = doc["isConfigured"] | false;
  const String version = doc["version"] | "";
  // An empty version answers "no build is configured" identically to
  // isConfigured being false - checked separately anyway, since a
  // manifest that is misconfigured to report isConfigured=true with no
  // version must not be read as "always different, always update".
  const bool isNewer = isConfigured && version.length() > 0 && version != Identity::installedAppVersion();
  Log::printf("[update] manifest check: configured=%d version=%s installed=%s -> %s", isConfigured,
              version.c_str(), Identity::installedAppVersion().c_str(),
              isNewer ? "update available" : "up to date");
  return isNewer;
}

}  // namespace AppUpdater
