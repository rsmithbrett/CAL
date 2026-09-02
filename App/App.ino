// App - the first real content the loader installs.
//
// Runs from ota_0, installed and started by CAL (see ../CAL.ino,
// ../Updater.cpp). Everything CAL already established - WiFi credentials, the
// device secret, TLS trust - lives in NVS and is simply read here, not
// re-derived. This build renders one card, weather; see Weather.h for why
// additional cards are meant to become sibling files rather than growing this
// one.
//
// Unlike CAL, this is meant to run indefinitely: failures here retry instead
// of halting, because a display that goes dark until someone finds a USB
// cable is a worse outcome for a household than one that keeps trying.

#include <WiFi.h>

#include "AppUpdater.h"
#include "Config.h"
#include "Display.h"
#include "Identity.h"
#include "Loader.h"
#include "WifiJoin.h"
#include "AppService.h"
#include "Weather.h"

namespace {

// Same pin and hold time as CAL's own WiFi-reset gesture, and deliberately so
// - a household should not need to know which binary happens to be running to
// know how to fix "wrong network".
constexpr uint8_t kBootButtonPin = 0;
constexpr uint32_t kWifiResetHoldMs = 3000;

bool wifiResetRequested() {
  pinMode(kBootButtonPin, INPUT_PULLUP);
  if (digitalRead(kBootButtonPin) != LOW) {
    return false;
  }

  Display::showStatus("Keep holding BOOT to reset WiFi", "Release now to cancel");
  const uint32_t deadline = millis() + kWifiResetHoldMs;
  while (millis() < deadline) {
    if (digitalRead(kBootButtonPin) != LOW) {
      return false;
    }
    delay(50);
  }
  return true;
}

/// Blocks until WiFi is up, retrying indefinitely rather than giving up - the
/// App has no captive-portal fallback of its own (see WifiJoin.h), so the only
/// way out of "nothing remembered works" is the BOOT-hold gesture above,
/// which only takes effect at boot. A network that comes back on its own
/// (router reboot, brief outage) must not need that.
void ensureWifiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  while (!WifiJoin::joinStoredNetwork()) {
    Display::showFailure("Could not join WiFi",
                         "Hold BOOT for 3 seconds to set up WiFi again.");
    delay(30000);
  }
}

uint32_t lastContentFetchMs = 0;
uint32_t lastUpdateCheckMs = 0;

void refreshWeatherCard() {
  const Weather::Result result = Weather::fetchMine();

  if (result.status == Weather::Status::Ok) {
    Display::showWeatherCard(result.forecast.location, result.forecast.temperature,
                             result.forecast.unit, result.forecast.shortForecast,
                             "Updated just now");
    return;
  }

  // NotActivated and ProviderDisabled are resting states an owner or their
  // agent can change server-side at any time - shown as status, not failure,
  // since there is nothing wrong with the device itself.
  const bool isRestingState = result.status == Weather::Status::NotActivated ||
                              result.status == Weather::Status::ProviderDisabled;
  if (isRestingState) {
    Display::showStatus("Weather is not showing yet", result.message);
  } else {
    Display::showFailure("Could not load weather", result.message);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);

  Display::begin();
  Display::showStatus("Starting", "");

  Identity::begin();

  if (wifiResetRequested()) {
    Loader::returnToLoaderForReprovisioning();
    // Unreachable: the call above never returns.
  }

  ensureWifiConnected();
  WiFi.setAutoReconnect(true);

  // Reaching a working network is this build's definition of steady state -
  // see Identity.h's own remarks on bootAttempts for why leaving this
  // uncleared would eventually make CAL treat a perfectly healthy App as
  // unbootable.
  Identity::clearBootAttempts();

  Display::showStatus("Checking the time", "Needed before a secure connection");
  while (!AppService::synchroniseTime()) {
    Display::showFailure("Cannot reach the internet", "Retrying...");
    delay(10000);
  }

  Display::showStatus("Loading weather", "");
  refreshWeatherCard();
  lastContentFetchMs = millis();
}

void loop() {
  ensureWifiConnected();

  const uint32_t now = millis();

  if (now - lastContentFetchMs >= Config::kContentRefreshIntervalMs) {
    refreshWeatherCard();
    lastContentFetchMs = now;
  }

  if (now - lastUpdateCheckMs >= Config::kUpdateCheckIntervalMs) {
    lastUpdateCheckMs = now;
    if (AppUpdater::newerVersionAvailable()) {
      Display::showStatus("Updating", "A new version is available");
      Loader::requestUpdate();
      // Unreachable: the call above never returns.
    }
  }

  delay(1000);
}
