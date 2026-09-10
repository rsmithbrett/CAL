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
#include <esp_bt.h>      // esp_bt_mem_release() - see releaseBluetoothMemory() below
#include <esp_system.h>  // esp_restart() - see checkHeapHealth() below

// Overrides the ESP32 Arduino core's weak getArduinoLoopTaskStackSize() (see
// cores/esp32/main.cpp), which otherwise sizes loopTask's stack at a fixed
// 8192 bytes no matter what a sketch actually does in loop(). Must be a
// plain global function, not inside any namespace - the core's own
// definition isn't extern "C", so the linker matches this override by exact
// mangled signature, and an anonymous-namespace or static definition here
// would mangle differently and silently fail to replace it.
//
// Raised from the 8192-byte default after a live device crashed with "Guru
// Meditation Error ... Stack canary watchpoint triggered (loopTask)" running
// the multi-instance card build: Forecast::fetch() (see Forecast.cpp) kept a
// NetworkClientSecure, an HTTPClient and two JsonDocuments alive across a TLS
// handshake and JSON parse, all as stack locals in one frame, and going from
// one forecast instance to up to five meant that TLS-handshake-heavy path got
// hit several times more often per rotation - turning an already marginal
// peak into one this device reached during a real overnight run. Doubling to
// 16384 leaves the same code path comfortable headroom without meaningfully
// denting the ~320KB of RAM this chip has.
//
// Partly stale as of Http.h/.cpp: the NetworkClientSecure and HTTPClient are
// no longer stack locals in Forecast::fetch() (or any of the other former
// per-call-site owners) - they are one shared pair living for the whole
// process (see Http.h's own remarks on why). That shrinks this frame's
// actual stack cost somewhat, but not enough of the original crash's
// reasoning to justify shrinking this constant back down without a live
// device to verify against: the two JsonDocuments this comment also names
// are untouched by that change, and 16384 costs this device nothing it would
// otherwise use. Left doubled, not tuned back down, until someone has actual
// headroom data from hardware running this change to act on.
size_t getArduinoLoopTaskStackSize(void) {
  return 16384;
}

#include "Actions.h"
#include "AppService.h"
#include "AppUpdater.h"
#include "Assets.h"
#include "CardManager.h"
#include "CheckIn.h"
#include "Config.h"
#include "Display.h"
#include "HomeValue.h"
#include "Http.h"
#include "Identity.h"
#include "IssFlyover.h"
#include "Loader.h"
#include "Log.h"
#include "SdStorage.h"
// Included for setPhase()/setTimes()/setPosition()/setValue() only, not to
// register any of these five cards - cards still register themselves at
// static-init time and App.ino names none of them. All five need a push
// because their content rides the check-in response rather than a fetch of
// their own.
#include "MoonPhase.h"
#include "SunMoon.h"
#include "Tides.h"
#include "Telemetry.h"
#include "WifiJoin.h"

namespace {

/// Hands the Bluetooth controller's reserved DRAM back to the general heap.
///
/// This firmware contains no Bluetooth code whatsoever - no BluetoothSerial,
/// no BLEDevice, nothing - but the ESP32 Arduino core links the BT stack in
/// regardless, and the controller's DRAM region stays RESERVED at boot until
/// an application explicitly releases it. A radio this device never turns on
/// was therefore holding tens of KB out of the heap on every boot, for the
/// entire uptime, on every device in the fleet.
///
/// Why this was worth finding: the symptom was not "out of memory" - free
/// heap looked fine at 60-70KB. It was that `ESP.getMaxAllocHeap()` (the
/// largest single CONTIGUOUS block) sat at exactly 32,756 bytes, boot after
/// boot, on hardware. LovyanGFX's PNG decoder needs roughly 44KB contiguous
/// for its scratch buffer, so every graphic card failed to decode
/// ("[display] failed to draw ... maxAllocHeap=32756"), dropped itself from
/// the rotation, and left the device cycling only the cards that need no
/// picture - while the heap-health watchdog restarted it every ~3 minutes for
/// a fragmentation it could never clear. A number that is constant to the
/// byte across dozens of reboots is the signature of a fixed reservation, not
/// of accumulated fragmentation, which is what pointed here.
///
/// ESP_BT_MODE_BTDM releases both the Classic and BLE regions - the whole
/// reservation, since neither is ever used. This is deliberately
/// irreversible: Bluetooth cannot be started again after this call without a
/// reboot, which costs this firmware nothing and is exactly the point.
///
/// Called as the very first thing in setup(), before Display/WiFi/TLS take
/// their own allocations, so everything after it is competing for a heap that
/// already includes this region rather than fitting around it.
///
/// Note for anyone revisiting Display.cpp's decision never to call
/// releasePngMemory(): its justification reads "CAL has no Bluetooth stack to
/// feed" (true of the source, false of the binary until this line existed).
/// That reasoning should be re-read now that this memory is actually free.
void releaseBluetoothMemory() {
  const uint32_t before = ESP.getMaxAllocHeap();
  const esp_err_t err = esp_bt_mem_release(ESP_BT_MODE_BTDM);
  const uint32_t after = ESP.getMaxAllocHeap();

  if (err == ESP_OK) {
    Log::printf(
        "[boot] released the unused Bluetooth controller's DRAM - maxAllocHeap %lu -> %lu bytes "
        "(freeHeap=%lu)",
        static_cast<unsigned long>(before), static_cast<unsigned long>(after),
        static_cast<unsigned long>(ESP.getFreeHeap()));
  } else {
    // Not fatal: the device runs exactly as it did before this existed, just
    // without the reclaimed region. Logged loudly rather than ignored,
    // because a silent failure here would look identical to the bug this
    // call exists to fix.
    Log::printf("[boot] could not release Bluetooth DRAM (esp_err=%d) - continuing without it",
                static_cast<int>(err));
  }
}

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

  Display::showStatus("Keep holding BOOT to set up WiFi", "Release now to cancel");
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
uint32_t lastHeapCheckMs = 0;

// ---------------------------------------------------------------------------
// Heap fragmentation watchdog.
//
// ESP.getFreeHeap() can stay generous (device 7 held ~85-140KB free all
// night) while ESP.getMaxAllocHeap() - the largest still-*contiguous* free
// block - keeps shrinking underneath it: enough small, long-lived
// allocations (TLS session state, JSON documents, the PNG decoder's own
// scratch buffer) scattered across an uptime eventually leave no single
// block big enough for the next large one, even though the sum of free
// memory looks fine. Observed live on device 7 tonight: maxAllocHeap fell
// from 42996 to 34804 bytes within about a minute of uptime. Once it drops
// below what a given asset's read buffer needs, that asset's card silently
// stops decoding (see Graphic.cpp's "would not decode" line and Assets.cpp's
// own out-of-memory logging) - the suspected, not yet proven, cause of this
// device's periodic reboot pattern. Nothing else in this firmware reads this
// value proactively today; Display.cpp and Assets.cpp only log it after an
// allocation has already failed. This check acts before that point,
// restarting deliberately - on this firmware's own terms, with a friendly
// on-screen message and time for the log line explaining why to actually
// reach the server - rather than waiting for a card to mysteriously vanish
// or an allocation to fail somewhere less recoverable.
//
// This is independent of Loader.cpp's restart paths (requestUpdate(),
// returnToLoaderForReprovisioning()): those hand control back to CAL in the
// factory partition for a reprovision or an OTA install. This restart has
// nothing to do with either - it only wants a clean heap for the *same*
// App image already running, so it calls esp_restart() directly here rather
// than routing through Loader.cpp.
constexpr uint32_t kHeapCheckIntervalMs = 60000;  // once a minute

// TLS handshakes and the rest of setup()'s own startup allocations
// legitimately dip maxAllocHeap during the first stretch of a boot, before
// the device has ever reached steady state - checking during that window
// risks restarting a perfectly healthy device before it gets there at all,
// which would turn this into the cause of a reboot loop rather than the fix
// for one. 3 minutes is comfortably past every boot-time allocation this
// firmware makes (WiFi join, SNTP, the first check-in, the first card
// fetches) - this is not a hypothetical margin: it is exactly the kind of
// mistake this codebase's own firmware-recovery procedure was caught making
// earlier tonight, restarting before the device had ever settled.
constexpr uint32_t kHeapCheckGraceMs = 3UL * 60UL * 1000UL;

// What "too fragmented to work" actually means, and why this is not 60000.
//
// It WAS 60000, chosen from a range (34804-42996) this was observed failing in
// before anyone knew what the steady state of a healthy device looked like.
// That number turned out to be unreachable by design, which made this
// watchdog the reboot loop it was written to prevent - device 17, healthy and
// drawing cards, restarting every three minutes to the tick:
//
//   [health] maxAllocHeap=32756 below 60000 byte threshold after 180011 ms
//            uptime - restarting to clear fragmentation
//
// maxAllocHeap sits pinned at exactly 32,756 from the first card draw onward,
// on every boot, and that is CORRECT: Display.cpp deliberately keeps
// LovyanGFX's ~44KB decode scratch for the whole uptime (see its
// releaseDecodeMemory() remarks for why releasing it breaks every graphic
// card), and a permanently-held block that size necessarily splits the heap.
// A threshold above the intended steady state is not a safety margin, it is
// an unconditional restart timer.
//
// So the threshold is derived from what a draw actually has to allocate
// instead of from one bad night's observations. The server normalizes every
// asset to at most 24KB precisely because of this device's contiguity ceiling
// (DiscoverAroundMe's AssetSizeTarget.MaxBytes documents the same 32,756
// figure from the other side), and a draw needs one block that size for the
// file copy. Below 28KB the largest permitted asset genuinely cannot be read
// and a card really would drop out silently - which is the condition worth
// restarting for. Above it, the device is doing exactly what it should.
//
// Keep this in step with AssetSizeTarget.MaxBytes on the server: this must
// stay above it, with room for the allocator's own overhead.
constexpr size_t kMinMaxAllocHeapBytes = 28000;

/// Checked once per loop() iteration, but only actually reads
/// ESP.getMaxAllocHeap() at most once every kHeapCheckIntervalMs - see the
/// block comment above for the grace period and threshold this compares
/// against.
void checkHeapHealth() {
  const uint32_t now = millis();
  if (now < kHeapCheckGraceMs) {
    return;
  }
  if (now - lastHeapCheckMs < kHeapCheckIntervalMs) {
    return;
  }
  lastHeapCheckMs = now;

  const size_t maxAllocHeap = ESP.getMaxAllocHeap();
  if (maxAllocHeap >= kMinMaxAllocHeapBytes) {
    return;
  }

  Log::printf(
      "[health] maxAllocHeap=%u below %u byte threshold after %lu ms uptime - restarting to "
      "clear fragmentation",
      static_cast<unsigned>(maxAllocHeap), static_cast<unsigned>(kMinMaxAllocHeapBytes),
      static_cast<unsigned long>(now));
  // So the line above actually reaches the server instead of being lost with
  // everything else in RAM at restart - same reasoning as every other
  // pre-esp_restart() call site in this codebase (see Loader.cpp).
  Log::flushNow();
  // The clock right now is already correct - this restart is deliberate and
  // self-inflicted, not a power loss, so there is a real reading worth
  // carrying into the next boot. See AppService::trySkipSyncAfterFastReboot()
  // for what this buys: skipping the blocking SNTP wait entirely on the very
  // next setup(), which is what most of this restart's own visible outage
  // window was actually spent on.
  AppService::stashTimeForFastReboot();
  Display::showStatus("Refreshing", "Reclaiming memory - back in a moment");
  // Long enough for both the status message and the flushed log line to be
  // visibly sent before the restart cuts everything off.
  delay(1500);
  esp_restart();
  // Unreachable: the call above never returns.
}

// The server can shorten or lengthen this on every check-in response
// (CheckInResponse.CheckInIntervalSeconds, resolved from the
// "checkin_interval_seconds" config key) - a fleet's polling cadence is an
// operational decision, not a constant this firmware should own.
//
// The value here is only what a boot starts with, before any check-in
// response has ever replaced it - and it can never itself come from the
// server, since a device that has never yet reached the server has nothing
// to ask. It used to be a flat 5 minutes, matching CheckInGatewayService's
// own DefaultIntervalSeconds. Found live: that meant a fresh boot could not
// discover its real card policy - or even that a check-in was failing at
// all - for a full 5 minutes, which is a bad way to spend the first minutes
// of testing a change, and separately left every registered card (all 28,
// every multi-instance forecast/graphic slot included) running in their
// default-active state for that whole window, adding real heap pressure on
// a board that already fragments (see the heap-health watchdog below) - a
// live device was caught restarting from fragmentation at almost exactly
// 180 seconds of uptime, boot after boot, because its real (much narrower)
// policy never had a chance to arrive before the watchdog's own 3-minute
// grace period ran out.
//
// setup() overwrites this with a random 0-90 second jitter before the
// first loop() iteration ever checks it - see kFirstCheckInMaxJitterMs
// below for why a jitter, not simply "as fast as possible".
uint32_t checkInIntervalMs = 5UL * 60UL * 1000UL;

// Bounds the jitter setup() applies to checkInIntervalMs for the very first
// check-in of a boot, before any server response has set a real interval.
// Not zero, on purpose: a fleet that all rebooted within the same few
// seconds of each other - the exact shape of an OTA rollout, the one
// moment a fleet's reboots are most correlated - would otherwise send every
// device's first check-in in that same instant. 90 seconds is short enough
// that "did this boot's check-in even succeed" is still answered in well
// under two minutes (the original complaint this replaces a flat 5-minute
// wait for), while still spreading a simultaneous fleet-wide reboot's first
// check-ins across a real window instead of one instant.
constexpr uint32_t kFirstCheckInMaxJitterMs = 90UL * 1000UL;

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
  // Same reasoning, same push, one card over: the Moon-phase card has no fetch of
  // its own either, and its content rides this exact response.
  MoonPhase::setPhase(result.moonPhase, result.moonIlluminatedFraction, result.moonPhaseName);
  // Same reasoning, same push, one card over again: the tides card has no fetch
  // of its own either, and its content rides this exact response too.
  Tides::setTimes(result.nextHighTideMinutesUtc, result.nextLowTideMinutesUtc,
                   result.utcOffsetMinutes);
  // Same reasoning, same push, one card over again: the home value card has
  // no fetch of its own either, and its content rides this exact response
  // too - see HomeValue.h.
  HomeValue::setValue(result.homeValueEstimate, result.homeValueRangeLow, result.homeValueRangeHigh,
                      result.homeValuePricePerSquareFoot, result.homeValueUpdatedAtUtc);
  // Same reasoning, same push, one card over again: the ISS flyover card has
  // no fetch of its own either, and its content rides this exact response
  // too.
  IssFlyover::setPosition(result.issLatitude, result.issLongitude, result.issDistanceMiles,
                          result.issBearingDegrees);
  // Same reasoning, same push, one card over again, for that card's other
  // display mode - the next predicted pass, shown when there is no live
  // position (see IssFlyover.h). Found live tonight: the server side of
  // this shipped earlier this session but nothing ever called setNextPass().
  IssFlyover::setNextPass(result.issNextPassRiseUtc, result.issNextPassRiseAzimuthDegrees,
                          result.issNextPassMaxElevationUtc, result.issNextPassMaxElevationDegrees,
                          result.issNextPassMaxElevationAzimuthDegrees, result.issNextPassSetUtc,
                          result.issNextPassSetAzimuthDegrees, result.utcOffsetMinutes);

  // Not a card push like everything above - there is no splash card, and
  // nothing here redraws the screen. When the account has a boot splash
  // configured, this fetches and SHA-256-verifies it into the fixed "splash"
  // SD slot via the same Assets machinery every other cached picture uses
  // (see Assets.h's ensureSplashCached()), doing nothing at all when the id
  // has not changed since the last time this succeeded. This check-in cannot
  // put the new splash on screen right now: showBootSplash() only ever runs
  // once, at boot, before this run's first check-in - so a freshly-configured
  // splash first appears on the *next* boot, not this session.
  if (result.splashAssetId.length() > 0) {
    Assets::ensureSplashCached(result.splashAssetId);
  }

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

  // One-shot, like updateAvailable below - the server already cleared its
  // own copy of this flag the moment it answered true (see CheckIn.h's own
  // remarks), so this fires exactly once per admin button press regardless
  // of how many check-ins land before the next one. Deletes every cached
  // image; nothing here forces an immediate re-fetch - the ordinary refresh
  // sweep (refreshOneDueCard()) notices the cache miss on each card's own
  // next due cycle and re-downloads through the same SHA-256-verified path
  // as any other cache miss, so a card may show no picture for up to one
  // refresh interval rather than the request blocking on several fetches
  // back to back.
  if (result.sdReformatRequested) {
    Log::line("[checkin] server requested an SD card reformat");
    const uint16_t removed = Assets::wipeCache();
    Log::printf("[checkin] SD reformat complete (%u file(s) removed)", removed);
  }

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

  // First, before anything else allocates: hand back the DRAM the unused
  // Bluetooth controller reserves at boot. See releaseBluetoothMemory()'s own
  // remarks - this is what was capping maxAllocHeap below what the PNG
  // decoder needs, and it has to happen before Display/WiFi/TLS carve up
  // what is left.
  releaseBluetoothMemory();

  Display::begin();
  Display::showStatus("Starting", "");

  Identity::begin();

  // Storage and the boot splash come up HERE - before Http/WiFi/TLS - and the
  // ordering is the whole fix, not a cosmetic preference.
  //
  // Measured on hardware: maxAllocHeap is 110,580 bytes at this point in boot
  // and only 32,756 by the time the network stack has finished allocating.
  // LovyanGFX's PNG decoder needs roughly 44KB contiguous for its scratch
  // buffer, and readFileToBuffer() takes another ~52KB for the file itself
  // (see Display.cpp's own remarks on both). Drawing the splash after WiFi
  // therefore asked for 44KB out of 32,756 and failed every single time -
  // silently, because showBootSplash() ignores the result and boot-time logs
  // never reach the remote debug stream. Every graphic card then failed the
  // same way for the rest of the run, dropped itself from the rotation, and
  // left the device cycling only the cards that need no picture.
  //
  // Drawing it here instead succeeds, and the *reason it keeps working* is
  // Display.cpp's deliberate decision never to call releasePngMemory(): the
  // decoder's scratch buffer, allocated once here while memory is plentiful,
  // stays allocated for the whole uptime. Every later card draw reuses it
  // rather than trying to claw 44KB out of a heap the network stack has
  // already carved up. That decision looked like the liability; it is
  // actually what makes this work, provided the FIRST decode happens early.
  //
  // Streaming from SD instead (lcd.drawPngFile(), which is what CYD-Dickey
  // does and why it never hit this) is deliberately NOT the fix here: this
  // board is an LCDWIKI E32R28T whose SD card shares SCLK/MISO/MOSI with the
  // display by the manufacturer's own documentation - see README's "The
  // likely root cause" section. readFileToBuffer() exists precisely to keep
  // the SD read and the display writes from interleaving on that shared bus.
  //
  // Also, incidentally, what the splash was always meant to do: put the logo
  // up first and let WiFi connect underneath it, rather than showing it after
  // the network is already up.
  //
  // Note on file timestamps: Sd::begin() used to run after time sync so a
  // freshly-cached asset got a plausible modification time. Nothing is cached
  // during setup() - showBootSplash() only reads - and every real cache write
  // happens on check-in, long after the clock is set, so that property is
  // unaffected.
  Sd::begin();
  Assets::begin();

  // Whether the logo actually reached the screen. When it did, the status
  // screens below ("Checking the time", "Loading") are skipped so it stays up
  // for the whole of WiFi join and time sync, rather than being painted over
  // a moment after appearing - the branded boot this feature exists for, and
  // what the splash was always meant to do: the logo first, the network
  // connecting underneath it.
  //
  // A device with no splash - no card, nothing configured, or a decode that
  // failed - falls back to those status screens exactly as before. Silence on
  // a blank panel while WiFi retries is a worse boot than a plain status line,
  // so this only ever suppresses them when there is genuinely something better
  // on screen. Failure screens (see ensureWifiConnected) are never suppressed:
  // a household that needs to hold BOOT to fix its WiFi has to be told so.
  const bool splashOnScreen = Assets::showBootSplash();

  // Configures the one shared HTTPS connection's TLS trust bundle exactly
  // once for this boot - see Http.h's own remarks for why every HTTP call
  // site now shares one persistent NetworkClientSecure/HTTPClient pair
  // instead of building its own. Needs no network of its own (it only
  // attaches this device's baked-in cert bundle) so it can run this early,
  // well before WiFi or the first check-in that will actually use it.
  Http::begin();

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

  // See checkInIntervalMs's and kFirstCheckInMaxJitterMs's own remarks above
  // for why this boot's first check-in fires from a random short interval
  // rather than the old flat 5-minute default. esp_random() is the ESP-IDF
  // hardware RNG - available this early, and true entropy rather than
  // something that would need seeding from a value this device does not
  // have yet anyway.
  checkInIntervalMs = esp_random() % kFirstCheckInMaxJitterMs;
  Log::printf("[boot] first check-in in %lu ms (jittered, not the old flat 5 minutes)",
              static_cast<unsigned long>(checkInIntervalMs));

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

  // A boot that immediately follows this same App's own heap-health restart
  // already has a clock that was correct moments ago - see
  // trySkipSyncAfterFastReboot()'s own remarks. Skips the blocking wait
  // below entirely when that is true; falls through to the ordinary
  // check-the-time screen otherwise (a genuine cold boot, a fresh flash, or
  // handing back from CAL after an OTA install - none of which have
  // anything to restore).
  if (!AppService::trySkipSyncAfterFastReboot()) {
    if (!splashOnScreen) {
      Display::showStatus("Checking the time", "Needed before a secure connection");
    }
    while (!AppService::synchroniseTime()) {
      Display::showFailure("Cannot reach the internet", "Retrying...");
      delay(10000);
    }
  }

  // Storage and the splash now come up much earlier, before Http/WiFi/TLS -
  // see the block above Http::begin() for why that ordering is load-bearing
  // rather than cosmetic. Storage itself remains optional: a device with
  // nothing in the card slot mounts nothing, caches nothing, reports zeroes
  // in telemetry and otherwise behaves identically (see SdStorage.h).

  // Hands the screen over. Every card registered itself before setup() was
  // ever called; this is where the rotation starts running.
  //
  // "Loading" is skipped while the logo is up, for the same reason the time
  // screen above is: CardManager::poll() holds whatever is on screen until a
  // real policy arrives (see its own gPolicyEverApplied remarks), so leaving
  // the splash there means the logo stays put right up until the first real
  // card replaces it - instead of a blank "Loading" filling that gap.
  if (!splashOnScreen) {
    Display::showStatus("Loading", "");
  }
  CardManager::begin();
}

void loop() {
  ensureWifiConnected();

  // Independent of every other timer in this loop, and checked early - see
  // checkHeapHealth()'s own remarks for why this restarts the App directly
  // rather than routing through Loader.cpp.
  checkHeapHealth();

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
