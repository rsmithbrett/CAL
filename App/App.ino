// App - the first real content the loader installs.
//
// Runs from ota_0, installed and started by CAL (see ../CAL.ino,
// ../Updater.cpp). Everything CAL already established - WiFi credentials, the
// device secret, TLS trust - lives in NVS and is simply read here, not
// re-derived.
//
// **This file does not know what a weather card is.** It brings up the
// hardware and the network, hands control of the screen to CardManager, and
// pumps it once per loop() iteration. Which cards exist, what order they
// appear in and how long each stays up are all decided elsewhere: each card
// module registers its own descriptor at static-init time (see the bottom of
// Weather.cpp and Aircraft.cpp), and the server's cardPolicy schedules them.
// This file used to hold an `enum class CardKind { Weather, Aircraft }` and a
// two-way toggle, which is exactly the thing that made adding a third card
// mean editing three unrelated places.
//
// Unlike CAL, this is meant to run indefinitely: failures here retry instead
// of halting, because a display that goes dark until someone finds a USB
// cable is a worse outcome for a household than one that keeps trying.

#include <WiFi.h>

#include "Actions.h"
#include "AppService.h"
#include "AppUpdater.h"
#include "Assets.h"
#include "CardManager.h"
#include "CheckIn.h"
#include "Config.h"
#include "Display.h"
#include "Identity.h"
#include "Loader.h"
#include "Log.h"
#include "SdStorage.h"
// Included for setTimes() only, not to register the card - cards still register
// themselves at static-init time and App.ino names none of them. This one needs a
// push because its content rides the check-in response rather than a fetch of its own.
#include "SunMoon.h"
#include "Telemetry.h"
#include "WifiJoin.h"

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
// (CheckIn::Result::utcOffsetMinutes/isDaytime) and both kept live here the
// same way checkInIntervalMs above already is - a value the server hands
// back on one successful check-in needs to keep being true in between check-
// ins, not just for the one loop() iteration it arrived on. Display.cpp keeps
// its own copy of both (see Display::setEnvironment()) since it's the thing
// that actually draws with them; App.ino's copies exist so a later check-in
// has something to compare against and so this state lives in exactly one
// kind of place in this file, alongside every other check-in-derived value.
//
// The offset alone is additionally mirrored to NVS (Identity::
// lastUtcOffsetMinutes()/setLastUtcOffsetMinutes()) - unlike checkInIntervalMs
// and lastIsDaytime, which only need to survive between check-ins, this one
// also needs to survive a *reboot* that happens before the first check-in of
// the new run has completed (WiFi still joining, a power cycle, etc.). Both
// variables start at the in-RAM defaults below (UTC, daytime); setup()
// overwrites the offset with whatever NVS remembers, if anything, before the
// first card ever draws - see its own call to Identity::lastUtcOffsetMinutes().
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
  // Puts the card that was showing back, rather than advancing to the next
  // one - a check that found nothing should leave the screen exactly as it
  // was, and this is a redraw of retained state, not a refetch.
  CardManager::redraw();
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
  // Persisted so the value survives a reboot - see Identity::lastUtcOffsetMinutes()'s
  // own remarks. Written on every successful check-in, not just the first, same as
  // the RAM copy above: NVS wear from one small write per checkInIntervalMs (minutes
  // apart, not milliseconds) is not a concern this firmware needs to manage.
  Identity::setLastUtcOffsetMinutes(lastUtcOffsetMinutes);

  // Same "current as of this check-in" contract as the two above, and pushed for
  // the same reason: the check-in path is the only thing that knows these, and
  // the sun card has no fetch of its own to pull them with.
  SunMoon::setTimes(result.sunriseMinutesUtc, result.sunsetMinutesUtc, result.utcOffsetMinutes);

  // The three card fields, in the order they have to happen in.
  //
  // acceptedActionIds is consumed first: it acknowledges presses this very
  // request carried, and clearing them before anything else can go wrong is
  // the point of the handshake. It is the one-shot-consume shape
  // FirmwareUpdateForced already uses, running the other way - the device
  // keeps carrying a press until the server says it has it, so a lost
  // response costs a duplicate send (which server-side dedup absorbs on
  // instanceId) rather than a lost press.
  Actions::clearAccepted(result.acceptedActionIds, result.acceptedActionCount);

  // Then the button set, before the policy, so the redraw the policy may
  // trigger already has the right buttons to draw. Not one-shot: the server
  // re-sends the full set every time, and an empty set is a legitimate
  // instruction meaning "no card draws any buttons".
  Actions::applyDefinitions(result.cardActions, result.cardActionCount);

  // Then the rotation itself. A response with no cardPolicy leaves whatever
  // is already in force alone - see CardManager::applyPolicy().
  CardManager::applyPolicy(result.cardPolicy);

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

  // Seeded from NVS before WiFi, time sync, or the first check-in - all of
  // which can take a while, or fail and retry, on a device that just powered
  // on. Without this the corner clock would draw raw UTC (the in-RAM default
  // above) for that whole stretch instead of the last offset this device
  // ever actually confirmed. A never-checked-in unit reads 0 back (NVS empty),
  // which is the same UTC default it already drew before this existed - not a
  // new failure mode, just not a worse one either.
  lastUtcOffsetMinutes = Identity::lastUtcOffsetMinutes();
  Display::setEnvironment(lastUtcOffsetMinutes, lastIsDaytime);

  // Once per start, before anything below can reboot or hand back to CAL, so
  // every boot is counted exactly once - including the ones that never get far
  // enough to report telemetry. Must stay in setup() and out of loop().
  Identity::recordBoot();

  Log::printf("[boot] App starting, installed version=%s totalBoots=%lu",
              Identity::installedAppVersion().c_str(),
              static_cast<unsigned long>(Identity::totalBoots()));

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

  // Storage is optional. A device with nothing in the card slot mounts
  // nothing, caches nothing, reports zeroes in telemetry and otherwise
  // behaves identically - see SdStorage.h. Brought up after the clock so a
  // just-cached asset gets a plausible modification time.
  Sd::begin();
  Assets::begin();
  Assets::showBootSplash();

  // Hands the screen over. Every card registered itself before setup() was
  // ever called; this is where the rotation starts running.
  Display::showStatus("Loading", "");
  CardManager::begin();
}

void loop() {
  ensureWifiConnected();

  if (forceUpdateCheckRequested()) {
    forceUpdateCheck();
    lastUpdateCheckMs = millis();
  }

  // Everything about what is on the screen - the dwell timer, the
  // interstitial interleaving, touch-driven forward/reverse and its
  // manual-nav hold, and refreshing at most one due card per pass - happens
  // in here. There is no content-refresh timer in this file any more; the
  // scheduler owns its own, per card.
  CardManager::poll();

  const uint32_t now = millis();

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

  // 50ms, not the 1000ms this loop used to sleep for. Everything else in
  // here is gated on its own millis() comparison and does not care how often
  // it is asked, but touch is sampled inside CardManager::poll() and a
  // once-per-second sample misses most of a real tap - a finger is on the
  // glass for a fraction of that. That was already true when a tap only
  // advanced a card, where a missed tap costs nothing worse than tapping
  // again; it is much less acceptable now that a tap can be a button press
  // whose whole point is that the person gets no confirmation and would
  // therefore never know it had not registered. CYD-Dickey's own loop() has
  // no delay in it at all for the same reason.
  delay(50);
}
