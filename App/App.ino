// App - the first real content the loader installs.
//
// Runs from ota_0, installed and started by CAL (see ../CAL.ino,
// ../Updater.cpp). Everything CAL already established - WiFi credentials, the
// device secret, TLS trust - lives in NVS and is simply read here, not
// re-derived. This build renders two cards, weather and aircraft overhead,
// alternating on the same content-refresh timer (see refreshCurrentCard());
// see Weather.h/Aircraft.h for why each card is its own sibling file rather
// than growing one another.
//
// Unlike CAL, this is meant to run indefinitely: failures here retry instead
// of halting, because a display that goes dark until someone finds a USB
// cable is a worse outcome for a household than one that keeps trying.

#include <WiFi.h>

#include "Aircraft.h"
#include "AppUpdater.h"
#include "CheckIn.h"
#include "Config.h"
#include "Display.h"
#include "Identity.h"
#include "Loader.h"
#include "Log.h"
#include "WifiJoin.h"
#include "AppService.h"
#include "Telemetry.h"
#include "Touch.h"
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
  Log::line("[boot] WiFi reset gesture confirmed");
  return true;
}

/// Blocks until WiFi is up, retrying indefinitely rather than giving up - the
/// App has no captive-portal fallback of its own (see WifiJoin.h), so the only
/// way out of "nothing remembered works" is the BOOT-hold gesture above -
/// which, unlike the one-shot check in setup(), stays live for the whole
/// time this function is stuck retrying (see below). A network that comes
/// back on its own (router reboot, brief outage) must not need that.
///
/// Bug fixed 2026-09-03: this loop used to `delay(30000)` between join
/// attempts without ever looking at the BOOT button, so a household holding
/// BOOT for 3 seconds - exactly what the "Could not join WiFi" screen below
/// tells them to do - while already stuck here did nothing at all. The
/// gesture only worked at the single instant setup() happened to call
/// wifiResetRequested() once, before this loop ever started; a device that
/// passed that check and only lost WiFi afterwards had no working recovery
/// gesture short of a precisely-timed power-cycle-and-immediately-hold. This
/// was found live on a physical device. Reusing wifiResetRequested() itself
/// - rather than writing a second, slightly different 3-second-hold
/// implementation here - means both call sites share one definition of "was
/// the gesture actually completed."
void ensureWifiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  while (!WifiJoin::joinStoredNetwork()) {
    Display::showFailure("Could not join WiFi",
                         "Hold BOOT for 3 seconds to set up WiFi again.");
    Log::line("[wifi] still not connected, retrying in 30s");

    // Same ~30s pace as the old delay(30000), but polled in short
    // increments so a hold started at any point during the wait - not just
    // at boot - actually reaches wifiResetRequested().
    const uint32_t waitDeadline = millis() + 30000;
    while (millis() < waitDeadline) {
      if (digitalRead(kBootButtonPin) == LOW) {
        if (wifiResetRequested()) {
          Loader::returnToLoaderForReprovisioning();
          // Unreachable: the call above never returns.
        }
        // Held briefly, then released before the 3-second confirm
        // completed - wifiResetRequested() left its own "keep holding"
        // prompt on screen; put the real status back before continuing to
        // wait, so the screen never claims a hold is still in progress
        // when it isn't.
        Display::showFailure("Could not join WiFi",
                             "Hold BOOT for 3 seconds to set up WiFi again.");
      }
      delay(100);
    }
  }
}

uint32_t lastContentFetchMs = 0;
uint32_t lastUpdateCheckMs = 0;
uint32_t lastCheckInMs = 0;

// The server can shorten or lengthen this on every check-in response
// (CheckInResponse.CheckInIntervalSeconds, resolved from the
// "checkin_interval_seconds" config key) - a fleet's polling cadence is an
// operational decision, not a constant this firmware should own. 5 minutes
// only until the first real check-in response replaces it, matching
// CheckInGatewayService's own DefaultIntervalSeconds.
uint32_t checkInIntervalMs = 5UL * 60UL * 1000UL;

// The corner clock's offset and the day/night theme, both check-in-driven
// (CheckIn::Result::utcOffsetMinutes/isDaytime) and both persisted here the
// same way checkInIntervalMs above already is - a value the server hands
// back on one successful check-in needs to keep being true in between check-
// ins, not just for the one loop() iteration it arrived on. Defaults (UTC,
// daytime) hold until the first successful check-in ever completes, matching
// the server's own fallback for an unresolved location. Display.cpp keeps
// its own copy of both (see Display::setEnvironment()) since it's the thing
// that actually draws with them; App.ino's copies exist so a later check-in
// has something to compare against and so this state lives in exactly one
// kind of place in this file, alongside every other check-in-derived value.
int lastUtcOffsetMinutes = 0;
bool lastIsDaytime = true;

/// True for exactly one loop() iteration per physical press - edge-detected
/// against the previous iteration's reading, not just "is it down right now",
/// so holding the button doesn't fire this repeatedly. Distinct from
/// wifiResetRequested()'s 3-second hold: that gesture only runs once, at
/// boot, before the button is ever read again here - a plain press during
/// normal operation was otherwise unused and is free to mean something else.
bool forceUpdateCheckRequested() {
  static bool wasPressed = false;
  const bool isPressed = digitalRead(kBootButtonPin) == LOW;
  const bool justPressed = isPressed && !wasPressed;
  wasPressed = isPressed;
  return justPressed;
}

void refreshWeatherCard() {
  const Weather::Result result = Weather::fetchMine();

  if (result.status == Weather::Status::Ok) {
    Log::printf("[weather] card updated: %s, %d%s, %s", result.forecast.location.c_str(),
                result.forecast.temperature, result.forecast.unit.c_str(),
                result.forecast.shortForecast.c_str());
    Display::showWeatherCard(result.forecast.location, result.forecast.temperature,
                             result.forecast.unit, result.forecast.shortForecast,
                             "Updated just now");
    return;
  }

  // NotActivated and ProviderDisabled are resting states an owner or their
  // agent can change server-side at any time - shown muted, not amber, since
  // there is nothing wrong with the device itself. Routed through
  // showWeatherStatus() rather than the black boot-ladder showStatus()/
  // showFailure() - see Display.h's remarks on why content-card problems
  // stay in the white/bannered card family instead.
  const bool isRestingState = result.status == Weather::Status::NotActivated ||
                              result.status == Weather::Status::ProviderDisabled;
  Display::showWeatherStatus(isRestingState ? "Weather is not showing yet" : "Could not load weather",
                             result.message, /*isProblem=*/!isRestingState);
}

void refreshAircraftCard() {
  const Aircraft::Result result = Aircraft::fetchMine();

  if (result.status == Aircraft::Status::Ok) {
    Log::printf("[aircraft] card updated: %s alt=%dft speed=%.0fkts heading=%.0f dist=%.1fmi",
                result.nearest.callsign.c_str(), result.nearest.altitudeFeet,
                result.nearest.speedKnots, result.nearest.headingDegrees,
                result.nearest.distanceMiles);
    Display::showAircraftCard(result.nearest.callsign, result.nearest.altitudeFeet,
                              result.nearest.speedKnots, result.nearest.headingDegrees,
                              result.nearest.distanceMiles, "Updated just now");
    return;
  }

  // Empty (fetch worked, nothing in range right now) and the two resting
  // states read as ordinary/muted; auth and network trouble read amber -
  // same isProblem split refreshWeatherCard() makes, just with a third
  // muted case this card has and weather doesn't (weather's "no forecast
  // yet"/"no address set" cases are folded into NetworkError instead - see
  // Weather.cpp).
  const bool isRestingState = result.status == Aircraft::Status::NotActivated ||
                              result.status == Aircraft::Status::ProviderDisabled ||
                              result.status == Aircraft::Status::Empty;
  const String headline = result.status == Aircraft::Status::Empty ? "Nothing overhead right now"
                          : isRestingState                          ? "Aircraft overhead is not showing yet"
                                                                     : "Could not load aircraft data";
  Display::showAircraftStatus(headline, result.message, /*isProblem=*/!isRestingState);
}

// Weather and aircraft overhead alternate on the same content-refresh timer
// rather than each getting a card-cycling scheduler of its own the way
// CYD-Dickey's drawDashboardScreen()/advanceCard() interleave weather,
// splash, QR and a whole list of aircraft/listings on independent
// intervals with touch-driven rewind. Most of that machinery still has
// nothing to attach to here: the server gives CAL only the *nearest*
// aircraft as a single featured value, not a list to cycle through, so
// there remains exactly one of each card. A tap (see Touch.h/.cpp, wired up
// in loop()) now advances between them immediately instead of only ever
// waiting for the timer - the board's touch controller was simply unused
// before this, not absent - but "the next card" is still unambiguous
// either way, so this stays a plain two-way toggle rather than a real
// cycling scheduler with history/rewind.
enum class CardKind { Weather, Aircraft };
CardKind nextCard = CardKind::Weather;

void refreshCurrentCard() {
  if (nextCard == CardKind::Weather) {
    refreshWeatherCard();
    nextCard = CardKind::Aircraft;
  } else {
    refreshAircraftCard();
    nextCard = CardKind::Weather;
  }
}

/// Checks right now instead of waiting for the button-triggered path's only
/// alternative (AppUpdater's own slow, independent manifest poll), and says
/// so on screen either way - a press that silently does nothing reads as a
/// dead button, and "nothing happened" and "already checked, nothing new"
/// look identical without this. This is the manual, at-the-device path;
/// performCheckIn() below is the automatic, server-driven one a "Force
/// update" admin button actually reaches.
void forceUpdateCheck() {
  Log::line("[update] manual check requested (BOOT press)");
  Display::showStatus("Checking for update", "");
  if (AppUpdater::newerVersionAvailable()) {
    Log::line("[update] manual check found a newer version");
    Display::showStatus("Updating", "A new version is available");
    Loader::requestUpdate();
    // Unreachable: the call above never returns.
  }

  Log::line("[update] manual check: already up to date");
  Display::showStatus("Already up to date", "");
  delay(1500);
  refreshCurrentCard();
}

/// The fast path: whatever the server decided on this check-in - an admin's
/// "Force update" button, or simply a newer build now marked current -
/// arrives here within one checkInIntervalMs, not AppUpdater's own slower
/// independent timer. An ordinary failed check-in (no ok, secret not
/// rejected) changes nothing; the next one is still checkInIntervalMs away,
/// same as a normal one. A check-in rejected for a stale secret is not
/// ordinary - see CheckIn.h's own remarks on secretRejected.
void performCheckIn() {
  const CheckIn::Result result = CheckIn::perform();
  if (!result.ok) {
    if (result.secretRejected) {
      Log::line("[checkin] secret rejected - device needs reprovisioning");
      Loader::returnToLoaderForReprovisioning();
      // Unreachable: the call above never returns.
    }
    return;
  }

  if (result.intervalMs > 0) {
    checkInIntervalMs = result.intervalMs;
  }

  // Not one-shot either, same reasoning as debugStreamRequested below: the
  // offset and daytime/nighttime state are both current-as-of-this-check-in
  // facts, not one-time settings, so every successful check-in refreshes
  // them rather than only the first one ever seen.
  lastUtcOffsetMinutes = result.utcOffsetMinutes;
  lastIsDaytime = result.isDaytime;
  Display::setEnvironment(lastUtcOffsetMinutes, lastIsDaytime);

  // Not one-shot, unlike updateAvailable below: this reflects the server's
  // current wish on every successful check-in, so remote debug streaming
  // turns on or off in step with an admin's toggle and recovers on its own
  // after a reboot within one check-in interval - see CheckIn.h's and Log.h's
  // own remarks.
  Log::setStreamingEnabled(result.debugStreamRequested);

  // Piggybacks on check-in's own cadence rather than owning a timer of its
  // own - see Telemetry.h for why riding this exact cadence (instead of a
  // slower, independent one) is what keeps the server's own staleness
  // threshold on /diag/telemetry meaningful. Sent here, before the
  // updateAvailable branch below, so a device about to reboot for an update
  // still leaves a fresh snapshot behind.
  Telemetry::report(result.updateAvailable ? "updateAvailable" : "ok");

  if (result.updateAvailable) {
    Log::line("[checkin] server requested an update - rebooting into CAL");
    Display::showStatus("Updating", "The server requested an update");
    Loader::requestUpdate();
    // Unreachable: the call above never returns.
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);

  Display::begin();
  Display::showStatus("Starting", "");

  Identity::begin();
  Log::printf("[boot] App starting, installed version=%s", Identity::installedAppVersion().c_str());

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
  refreshCurrentCard();
  lastContentFetchMs = millis();
}

void loop() {
  ensureWifiConnected();

  if (forceUpdateCheckRequested()) {
    forceUpdateCheck();
    lastContentFetchMs = millis();
    lastUpdateCheckMs = millis();
  }

  // Touch-driven card advance - the App's first use of the touch panel (see
  // Touch.h/.cpp). Resets lastContentFetchMs the same way the BOOT-press
  // path above does, so the automatic content-refresh timer's next fire is
  // pushed out from right now instead of landing moments later and
  // re-showing the very card this tap just requested (or worse, showing the
  // same card twice in a row instead of advancing to the other one).
  if (Touch::wasTapped()) {
    Log::line("[touch] tap detected - advancing to the next card");
    refreshCurrentCard();
    lastContentFetchMs = millis();
  }

  const uint32_t now = millis();

  if (now - lastContentFetchMs >= Config::kContentRefreshIntervalMs) {
    refreshCurrentCard();
    lastContentFetchMs = now;
  }

  if (now - lastCheckInMs >= checkInIntervalMs) {
    lastCheckInMs = now;
    performCheckIn();
  }

  // Belt-and-braces fallback only: performCheckIn() above is the fast path
  // that actually reaches an admin's "Force update" button or a fresh
  // version within one checkInIntervalMs. This independent, much slower
  // timer exists purely so an update is never permanently missed if
  // check-in itself were ever broken.
  if (now - lastUpdateCheckMs >= Config::kUpdateCheckIntervalMs) {
    lastUpdateCheckMs = now;
    if (AppUpdater::newerVersionAvailable()) {
      Log::line("[update] fallback timer found a newer version - rebooting into CAL");
      Display::showStatus("Updating", "A new version is available");
      Loader::requestUpdate();
      // Unreachable: the call above never returns.
    }
  }

  // Sends whatever debug-log lines have piled up since the last pass, when
  // remote streaming is currently on (see Log::setStreamingEnabled(), driven
  // by performCheckIn() above) - a no-op the rest of the time. Called once
  // per loop() iteration rather than on its own timer, same as every other
  // periodic thing in this loop.
  Log::poll();

  delay(1000);
}
