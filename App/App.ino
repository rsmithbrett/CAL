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
// heap_caps_get_largest_free_block() - the only heap figure in this file worth
// printing, and stated explicitly rather than relied on transitively through
// Arduino.h. Same include, for the same reason, as Http.cpp, Display.cpp and
// Telemetry.cpp: ESP's own wrappers overstate free memory by roughly 4x on this
// board, so nothing here should be able to reach them without also having this.
#include <esp_heap_caps.h>

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
#include "BootDiag.h"
#include "CardManager.h"
#include "CheckIn.h"
#include "Config.h"
#include "Display.h"
// For Graphic::releaseRamBuffers() on the check-in failure path. This is the
// only reason App.ino knows about a specific card module at all - every other
// card registers itself and is reached solely through CardManager - and it is
// here because the RAM asset buffers are the largest thing a card-less device
// holds, and freeing them is a recovery action rather than a card concern.
#include "Graphic.h"
#include "HomeValue.h"
#include "Http.h"
#include "Identity.h"
#include "IssFlyover.h"
#include "Loader.h"
#include "Log.h"
#include "SdStorage.h"
#include "StackWatch.h"
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

// ---------------------------------------------------------------------------
// What a boot is allowed to say on the glass.
//
// This device restarts itself far more often than its boot screens were
// designed for. Measured across the fleet on 2026-09-11: restarts every 13-27
// minutes, one unit 24 times in five hours. The boot sequence is therefore not
// a rare event a household sees once when they unbox the thing - on the worst
// devices it is most of what they ever see it do.
//
// The distinction that matters is the one BootDiag.h already draws: power was
// applied, versus this firmware restarted itself to get unstuck. They want
// opposite treatment on screen, and until now got identical treatment.
//
//   - Cold power-on. Somebody just plugged it in and is standing there waiting
//     to find out whether it works. "Looking for known networks", "Connecting
//     to WiFi", "Checking the time" are reassuring, and a silent panel would
//     read as a dead appliance. Narrate.
//
//   - Therapeutic restart. Nobody touched anything. The device was sitting on
//     a shelf showing cards and it blinked. Replaying the whole network
//     handshake turns an invisible self-heal into a visible fault - and at one
//     restart every quarter of an hour, into the device's dominant visual
//     behaviour. Do not narrate.
//
// **What a therapeutic boot shows instead is one screen, held - not a dark
// panel.** Darkness was considered and rejected: several seconds of black on a
// wall display reads as "the screen died", which is a worse lie than the one
// being fixed, and it would land every 13 minutes on exactly the devices
// already behaving worst. Redrawing the last card was also considered and is
// not possible - the card's content does not survive the restart, and by the
// time the App is running CAL has already repainted the panel anyway (every
// restart goes App -> CAL -> App; see BootDiag.h). So the rule is not "show
// nothing", it is "show one thing and stop changing it": a single static
// screen is indistinguishable from a picture that paused for a moment, which
// is the honest reading of a self-heal. A four-step ladder of network jargon
// is what makes it look like a fault.
//
// The "Starting" screen at the top of setup() is deliberately NOT suppressed,
// on a therapeutic boot or any other. It is not one of the messages this
// change exists to remove (it names no network, no clock and no server), it is
// what covers the gap while SD mounts and the splash decodes, and a device with
// a slow or failing card would otherwise sit on a blank panel for as long as
// that takes.
//
// Only LowHeap and Unreachable count as therapeutic. The other deliberate
// causes are somebody's request, not a device trying to fix itself:
// Reprovision means a household is standing at the device holding BOOT,
// SelfTest means an operator asked for a diagnostic build, and Ota is the one
// restart where a long wait is expected and legitimate - CAL runs its full
// download-and-install ladder in front of it, and if a new build fails to come
// up, the App's boot screens are the only visible evidence of how far it got.
// Hiding those would mean the one restart a person is actually waiting on
// became the least legible. An unexpected reset (panic, watchdog, brownout)
// narrates too, for a blunter reason: a crash is not therapy, this firmware
// did not choose it, and it has no basis for claiming the device is fine.
// ---------------------------------------------------------------------------

/// How many self-restarts in a row stay silent before the boot starts saying
/// something again.
///
/// **This is the floor under the silence, and it is the whole reason the
/// suppression above is defensible.** "Don't show noise" and "don't hide a
/// fault" are in genuine tension here: a device stuck in an unreachable loop
/// that silently swallows every restart is a device nobody ever learns is
/// unwell - it renders its cards perfectly, tells the household nothing, and
/// the only remaining evidence is a picture that blinks. Resolving that in
/// favour of silence forever would be trading a real diagnosis for a cosmetic
/// one.
///
/// Three, because the two cases are far apart in practice. A router hiccup or
/// one poisoned TLS session produces exactly one self-restart and then the
/// device runs for hours - that must never be narrated, and with a budget of
/// three it never is, not even if it happens twice more in the same day. A
/// device genuinely cycling at the observed 13-27 minute cadence spends three
/// restarts inside about an hour and starts explaining itself, which is soon
/// enough to be reported and late enough that nothing transient reaches it.
constexpr uint8_t kMaxSilentSelfRestarts = 3;

/// How long this boot has to last before the restart that caused it counts as
/// having worked, clearing the budget above.
///
/// Uptime rather than "a successful check-in" or "a successful card draw", and
/// that choice is deliberate: the two watchdogs fire on different symptoms, and
/// a device can satisfy either one's idea of healthy while still cycling on the
/// other's. A LowHeap unit checks in perfectly right up to the moment it cannot
/// draw; an Unreachable unit draws perfectly and cannot check in at all. The
/// only condition that means "the restart actually fixed it" for both is the
/// absence of the next restart, and the only way to observe an absence is to
/// wait.
///
/// An hour is comfortably past the worst observed cycle (a restart every 13-27
/// minutes), so a device in that state never reaches it and its count keeps
/// climbing, while a device that recovered clears on its first quiet hour.
constexpr uint32_t kSelfRestartRecoveredUptimeMs = 60UL * 60UL * 1000UL;

/// The shortest time the boot splash is allowed to be the thing on screen.
///
/// Reported by the household as "the splash is on screen for a split second".
/// Two separate things were doing that and both are fixed; this constant is the
/// second one.
///
/// The first was WifiJoin painting over it a few hundred milliseconds after it
/// appeared - see WifiJoin::setProgressVisible(), which is where the actual bug
/// was and why the README's claim that the logo "stays up through the WiFi
/// join" had never been true.
///
/// The second is that nothing ever guaranteed the splash any time at all. With
/// the join silenced, the splash lives until CardManager::begin() fetches and
/// draws the first card - and on a fast boot (a therapeutic restart skips the
/// SNTP wait entirely via trySkipSyncAfterFastReboot(), and WiFi can associate
/// in about a second) that is a very short life. A splash whose duration is
/// whatever the network happened to cost is not a boot experience, it is a race
/// the brand keeps losing. Three seconds is long enough to read as deliberate
/// from across a room and short enough that it is not felt as a delay; it is
/// measured from when the splash was drawn, so on any boot that already took
/// longer than this the wait is zero and nothing is slowed down at all.
constexpr uint32_t kMinSplashOnScreenMs = 3000;

/// Whether this boot narrates its progress on the glass. See the block comment
/// above for the decision; decideBootNarration() below is where it is made.
/// True until then, so anything drawn before the decision point (the "Starting"
/// screen) is unaffected.
bool gNarrateBoot = true;

/// When Assets::showBootSplash() put the logo up, or 0 if it never did. Read
/// only by holdSplash() below.
uint32_t gSplashDrawnAtMs = 0;

/// Latches once checkSelfRestartRecovery() has run, so the recovery check is a
/// once-per-boot event rather than something loop() re-evaluates forever after
/// the threshold passes.
bool gSelfRestartsCleared = false;

/// Whether wifiResetRequested() has put its "keep holding BOOT" prompt on the
/// screen. See restoreHeldBootScreen() below for the one thing that reads it
/// and why a quiet boot needs it at all.
bool gBootPromptDrawn = false;

/// Whether the restart that produced this boot was this firmware restarting
/// itself to recover, as opposed to a power-on, a crash, or a restart somebody
/// asked for. See the block comment above for why Ota, Reprovision and SelfTest
/// are deliberately excluded.
bool restartWasTherapeutic() {
  switch (BootDiag::lastRestartCause()) {
    case BootDiag::RestartCause::LowHeap:
    case BootDiag::RestartCause::Unreachable:
      return true;
    case BootDiag::RestartCause::None:
    case BootDiag::RestartCause::Ota:
    case BootDiag::RestartCause::Reprovision:
    case BootDiag::RestartCause::SelfTest:
      return false;
  }
  // Unreachable with the enum as it stands. Present so a cause added to
  // BootDiag.h later and not considered here defaults to being narrated - the
  // safe direction, since a boot that says too much is a nuisance and one that
  // hides a new failure mode is a bug nobody can see.
  return false;
}

/// Decides whether the rest of setup() talks, and says so in the log either
/// way.
///
/// Called once, immediately after the splash draw, because that is the first
/// moment both inputs exist - and because everything downstream of it (the WiFi
/// join, the time sync, "Loading") is exactly what is being decided about.
void decideBootNarration(bool splashOnScreen) {
  const bool therapeutic = restartWasTherapeutic();
  const uint8_t selfRestarts = Identity::consecutiveSelfRestarts();
  const bool budgetSpent = therapeutic && selfRestarts > kMaxSilentSelfRestarts;

  // A splash on the glass suppresses the ladder on ANY boot, cold ones
  // included - that predates this change and is what the splash feature was
  // always for (see Assets.h's showBootSplash() and the README's "Where the
  // boot splash has to happen"). What is new is that the suppression now
  // actually reaches WifiJoin, which is where it had been leaking.
  //
  // budgetSpent overrides both, splash or no splash. A device that has restarted
  // itself four times running has a fault worth more than a tidy logo.
  gNarrateBoot = budgetSpent || !(therapeutic || splashOnScreen);

  WifiJoin::setProgressVisible(gNarrateBoot);

  if (gNarrateBoot && gSplashDrawnAtMs != 0) {
    // A narrating boot paints over the splash within moments no matter what
    // this file does - WifiJoin is the very next thing to draw and it is not
    // suppressed here - so there is nothing left to hold, and holding whatever
    // replaced the logo would just add three seconds to a boot that is already
    // the loud kind. Forgetting the timestamp is how holdSplash() learns that.
    gSplashDrawnAtMs = 0;
  }

  Log::printf("[boot] boot progress screens %s (restart was %s, self-restarts in a row=%u/%u, "
              "splash on screen=%d)",
              gNarrateBoot ? "will be drawn" : "are SUPPRESSED - screen only, the log is unchanged",
              therapeutic ? "therapeutic" : "not therapeutic",
              static_cast<unsigned>(selfRestarts),
              static_cast<unsigned>(kMaxSilentSelfRestarts), splashOnScreen ? 1 : 0);

  if (!budgetSpent) {
    return;
  }

  // Past the budget, so this boot stops pretending. Said on the one screen that
  // is already going to be held for a while rather than added to the ladder as
  // yet another step - a household needs to be able to read it and repeat it to
  // whoever set the device up, which means it has to still be there a few
  // seconds later.
  //
  // Worded as a symptom, not a diagnosis. The device does not know whether this
  // is its own memory, the household's router, or the service being down, and a
  // screen that guesses wrong sends somebody to reset a router that was never
  // the problem.
  Log::printf("[boot] this device has restarted itself %u times in a row (budget %u) - saying so "
              "on screen rather than staying quiet about a restart that clearly is not working",
              static_cast<unsigned>(selfRestarts), static_cast<unsigned>(kMaxSilentSelfRestarts));
  Display::showStatus("Starting", "This device keeps restarting itself. If it continues, "
                                  "contact whoever set it up.");
}

/// A boot-ladder status screen that a quiet boot swallows.
///
/// The log line on the suppressed path is not decoration and must not be
/// removed to make a quiet boot quieter: the remote debug stream is the only
/// diagnostic channel a deployed device has, and "this screen was deliberately
/// withheld" has to be distinguishable there from "this stage never ran".
/// Nothing about what this firmware reports changes with this feature - only
/// what it paints.
void bootStatus(const String& headline, const String& detail) {
  if (!gNarrateBoot) {
    Log::printf("[boot] not drawing \"%s\" (%s) - boot progress is suppressed for this boot; "
                "see decideBootNarration()",
                headline.c_str(), detail.length() > 0 ? detail.c_str() : "no detail");
    return;
  }
  Display::showStatus(headline, detail);
}

/// Waits out whatever is left of kMinSplashOnScreenMs before the caller is
/// allowed to replace the splash.
///
/// A plain blocking wait, and that is fine here: this is the tail of setup(),
/// nothing else is running, touch is not sampled yet (CardManager::begin() has
/// not been called) and loop() has not started. It is the same shape as the
/// delay() the time-sync retry above already uses.
///
/// Logs the figure rather than only the decision, so it is answerable from the
/// field whether this minimum ever actually binds - if every device reports
/// waiting 0 ms the constant is dead weight, and if they all report waiting
/// most of it then boots are faster than this comment assumes.
void holdSplash() {
  if (gSplashDrawnAtMs == 0) {
    return;
  }
  const uint32_t shownForMs = millis() - gSplashDrawnAtMs;
  if (shownForMs >= kMinSplashOnScreenMs) {
    Log::printf("[boot] splash has been up %lu ms, past its %lu ms minimum - not waiting",
                static_cast<unsigned long>(shownForMs),
                static_cast<unsigned long>(kMinSplashOnScreenMs));
    return;
  }
  const uint32_t remainingMs = kMinSplashOnScreenMs - shownForMs;
  Log::printf("[boot] holding the splash another %lu ms (up for %lu of a %lu ms minimum) so it is "
              "part of the boot rather than a frame of it",
              static_cast<unsigned long>(remainingMs), static_cast<unsigned long>(shownForMs),
              static_cast<unsigned long>(kMinSplashOnScreenMs));
  delay(remainingMs);
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

  // Straight to Display, deliberately not through bootStatus() above: a finger
  // is on the button, so this is not boot progress being narrated at nobody -
  // it is the device answering a gesture in progress, and a quiet boot must
  // never make the one user-driven control on this appliance look dead. Same
  // reasoning that keeps the failure screens ungated.
  gBootPromptDrawn = true;
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

/// Puts back whatever a quiet boot was holding on screen after
/// wifiResetRequested() painted its prompt over it and the hold was then
/// abandoned.
///
/// **This is a cleanup that used to happen by accident and stopped.** The
/// "Keep holding BOOT" prompt is drawn the instant the button reads LOW and
/// stays there when the household lets go before three seconds. Nothing ever
/// cleared it - WifiJoin's "Looking for known networks" simply landed
/// milliseconds later and took the screen. Silencing that join is exactly what
/// this change does, so on a quiet boot the prompt would now sit there, telling
/// a household to keep holding a button they already released, until the first
/// card arrives some seconds later.
///
/// Does nothing on a narrating boot, where the accident still works and
/// re-decoding the splash for a screen about to be replaced anyway would only
/// cost the boot an SD read.
void restoreHeldBootScreen(bool splashOnScreen) {
  if (!gBootPromptDrawn) {
    return;
  }
  gBootPromptDrawn = false;
  if (gNarrateBoot) {
    return;
  }

  if (splashOnScreen && Assets::showBootSplash()) {
    // The minimum starts again from here, not from the original draw: the logo
    // was interrupted, and what a household actually saw of it is the stretch
    // that starts now.
    gSplashDrawnAtMs = millis();
    Log::line("[boot] BOOT was pressed and released during a quiet boot - the splash is back up");
    return;
  }

  Log::line("[boot] BOOT was pressed and released during a quiet boot - putting the \"Starting\" "
            "screen back over the abandoned hold prompt");
  Display::showStatus("Starting", "");
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
          // So the next boot reads SOFTWARE_RESET + REPROVISION - see BootDiag.h.

          BootDiag::recordRestartIntent(BootDiag::RestartCause::Reprovision);

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
        // This site does its own cleanup - the line above IS the restore - so
        // the flag is cleared here rather than left standing for
        // restoreHeldBootScreen(), which has already run by the time this loop
        // is reachable and would otherwise be looking at a prompt that is no
        // longer on screen.
        gBootPromptDrawn = false;
      }
      delay(100);
    }
  }
}

uint32_t lastUpdateCheckMs = 0;
uint32_t lastCheckInMs = 0;
uint32_t lastHeapCheckMs = 0;

// ---------------------------------------------------------------------------
// Cannot-draw watchdog. (It was the "heap fragmentation watchdog" for most of
// its life; the name changed because what it watches did.)
//
// The original reasoning, kept because it is where this started and because
// half of it is still true: ESP.getFreeHeap() can stay generous (device 7 held
// ~85-140KB free all night) while ESP.getMaxAllocHeap() - the largest
// still-*contiguous* free block - keeps shrinking underneath it, since enough
// small, long-lived allocations (TLS session state, JSON documents, the PNG
// decoder's own scratch buffer) scattered across an uptime eventually leave no
// single block big enough for the next large one even though the sum of free
// memory looks fine. Observed live on device 7: maxAllocHeap fell from 42996 to
// 34804 bytes within about a minute of uptime. Once the largest block drops
// below what an asset's read buffer needs, that asset's card silently stops
// decoding (see Graphic.cpp's "would not decode" line and Assets.cpp's own
// out-of-memory logging).
//
// What survives from that: free bytes and contiguous bytes really are different
// quantities on this board, and the second is the one that decides whether a
// card draws. What does not survive: the belief that ESP.getMaxAllocHeap()
// measures it. It does not - see kMaxConsecutiveDrawFailures below for the
// measurements that settled that, and for why this now acts on draws that
// actually failed instead of on any reading at all.
//
// Nor does the ambition of acting BEFORE the first failure, which is what the
// old thresholds were for. That is given up deliberately. Predicting the
// failure needs a trustworthy prediction, this board offers none, and three
// attempts at one produced a watchdog that rebooted healthy devices. Acting on
// the first few real failures instead costs a handful of missed card draws -
// which Assets.cpp already retries and recovers from - and in exchange the
// trigger cannot fire on a device that is working. Restarting deliberately, on
// this firmware's own terms with a friendly on-screen message and time for the
// log line explaining why to reach the server, is still much better than
// waiting for an allocation to fail somewhere less recoverable.
//
// This is independent of Loader.cpp's restart paths (requestUpdate(),
// returnToLoaderForReprovisioning()): those hand control back to CAL in the
// factory partition for a reprovision or an OTA install. This restart has
// nothing to do with either - it only wants a clean heap for the *same*
// App image already running, so it calls esp_restart() directly here rather
// than routing through Loader.cpp.
constexpr uint32_t kHeapCheckIntervalMs = 60000;  // once a minute

// Kept at 3 minutes, but for a different reason than it was chosen for.
//
// The original justification was that TLS handshakes and the rest of setup()'s
// startup allocations legitimately dip maxAllocHeap during the first stretch of
// a boot, before the device has ever reached steady state, so checking inside
// that window risked restarting a perfectly healthy device - which is exactly
// the mistake this codebase's own firmware-recovery procedure was caught making,
// restarting before the device had ever settled. That argument is now moot: the
// trigger is a run of real allocation failures, and a transient dip that no
// draw ever tripped over produces no failures to count.
//
// It stays because it does something else that is still needed. Together with
// kHeapCheckIntervalMs it bounds how often this can restart a device to roughly
// once every four minutes, which is the whole protection against a device that
// cannot draw even on a fresh heap turning into a tight reboot loop - and it
// guarantees that whatever failures a boot's first draws produce, the boot gets
// a full 3 minutes to also produce a success and clear them. 3 minutes is
// comfortably past every boot-time allocation this firmware makes (WiFi join,
// SNTP, the first check-in, the first card fetches).
constexpr uint32_t kHeapCheckGraceMs = 3UL * 60UL * 1000UL;

// Why this watchdog counts FAILURES and not free bytes, and the whole history
// of getting that wrong.
//
// Three thresholds have stood here, and all three were the same mistake:
//
//   60000  - taken from the range (34804-42996) the device was first seen
//            failing in, before anyone knew a healthy device's steady state.
//            ESP.getMaxAllocHeap() reports exactly 32,756 from the first card
//            draw onward, on every boot, so this threshold was unreachable and
//            the watchdog became an unconditional restart timer. Device 17 -
//            healthy, drawing cards - killed itself at 180 seconds to the tick:
//
//              [health] maxAllocHeap=32756 below 60000 byte threshold after
//                       180011 ms uptime - restarting to clear fragmentation
//
//            The explanation offered at the time was that 32,756 was CORRECT
//            and unavoidable, since Display.cpp deliberately keeps LovyanGFX's
//            ~44KB decode scratch for the whole uptime (see its
//            releaseDecodeMemory() remarks for why releasing it breaks every
//            graphic card) and a permanently-held block that size necessarily
//            splits the heap. That story fit, and it was wrong - see below. The
//            conclusion drawn from it, that a threshold above the intended
//            steady state is not a safety margin, survives its own reasoning.
//
//   28000  - derived from what a draw must allocate rather than from one bad
//            night, which sounded better: the server normalizes every asset to
//            at most 24KB for this exact ceiling (DiscoverAroundMe's
//            AssetSizeTarget.MaxBytes documents the same 32,756 figure from the
//            other side), so below 28KB the largest asset it may send cannot be
//            read. Sound arithmetic, wrong input - it was derived from
//            ESP.getMaxAllocHeap(), the same discredited metric as before.
//
//   28000, observe-only - the honest interim state while the metric itself was
//            under investigation. It logged and refused to act, because
//            restarting a household's display on a number nobody could explain
//            is worse than not restarting it at all.
//
// The investigation is now closed and this is what it found. Arduino's heap
// wrappers overstate the memory an allocation can reach by roughly 4x on this
// board. Measured on device 17 at one instant, while a 10,568-byte allocation
// was failing:
//
//   ESP.getFreeHeap()                        = 49,960
//   ESP.getMaxAllocHeap()                    = 32,756
//   heap_caps_get_free_size(8BIT)            = 11,340
//   heap_caps_get_largest_free_block(8BIT)   =  6,132
//   heap_caps_get_minimum_free_size          =  5,156
//   heap_caps_check_integrity_all            = OK
//
// All four capability classes (8BIT, INTERNAL|8BIT, DMA, DEFAULT) reported
// identical figures: no DMA starvation, no memory stranded in
// word-addressable-only regions, no corruption. And ESP.getMaxAllocHeap()'s
// 32,756 is 0x7FF4, twelve bytes short of 32KiB, reported identically on every
// device and every boot while freeHeap moves around it - the signature of a cap
// or a region boundary, not of a measurement. It is not reading the pool malloc
// draws from, so no threshold against it could ever have predicted a draw.
//
// **So this no longer thresholds anything.** It restarts on a run of
// consecutive failed DRAWS - Display::consecutiveDrawFailures().
//
// That sentence used to finish "incremented only when a file-buffer allocation
// has exhausted plain malloc, all three explicit capability sets, and the TLS
// release, and the draw is genuinely lost", and every clause of it described a
// mechanism that had already been deleted. There is no file buffer: draws
// stream from SD (commit 40440da), so there is no per-draw allocation left to
// exhaust anything, no capability-set escalation, and no TLS release on the
// draw path. The description is kept here in its corrected form rather than
// dropped because the wrong version was not harmless - it, and the old name on
// the constant below, are why a reading of 6/6 was taken for memory exhaustion
// for an entire evening on a device whose largest 8-bit block was 21,492 at the
// time.
//
// What the counter actually is: Display.cpp's noteDrawOutcome() ticks it once
// for every card draw where an image was in front of the decoder and no picture
// came out - a decode that failed, or a file whose bytes are not an image. Its
// noteNoImageAvailable() handles the case that is NOT counted, a draw that never
// reached a decoder because there was no image to give one. Both are internal to
// that file on purpose (the count is only meaningful if exactly one place
// decides what a failure is); see Display.h's consecutiveDrawFailures() for the
// contract this file actually depends on, why the exclusion exists, and what
// counting it cost. Acting on failed draws is better than any threshold for
// three reasons worth stating plainly:
//
//   - It measures the harm itself. "This device can no longer draw its cards"
//     is the condition worth restarting for, and this counts exactly that
//     rather than a quantity believed to correlate with it.
//   - It needs no theory about which pool is short, which capability class
//     matters, or how much overhead the allocator adds. Those questions cost
//     this project three wrong thresholds and several device-nights.
//   - It cannot be fooled by a metric that means something other than it
//     appears to. A wrapper reporting 4x the truth changes nothing about
//     whether malloc returned null.
//
// The largest-8BIT-block figure is still logged on every check, beside
// ESP.getMaxAllocHeap(), so the discrepancy stays visible in the field instead
// of becoming a claim in this comment - but neither number decides anything.

// The longest run of failures tolerated without restarting - so the 7th
// consecutive failure is the one that acts. Above one card's worth of retries,
// and deliberately so.
//
// Assets.cpp retries a failed decode kMaxDrawAttempts (3) times before giving
// up on an asset, invalidating its cache entry and re-fetching it - and each of
// those attempts runs the full read path, so one genuinely unlucky asset
// produces a run of 3. A limit of 3 or lower would let a single bad file reboot
// the device, which is both the wrong response (Assets.cpp's own
// invalidate-and-refetch is the right one) and a reboot loop waiting to happen
// if that asset is the one every rotation reaches.
//
// 6 is two full cards' worth. Exceeding it means every attempt on more than two
// assets failed with no success anywhere in between - the counter resets on ANY
// successful draw - which is the difference between "this asset is bad" and
// "this device cannot draw". Draws are seconds apart while a check runs once a
// minute, so a device in that state passes 6 well inside one interval and
// restarts on the very next check rather than much later.
//
// That last sentence is true of a device failing continuously and was read too
// broadly. Graphic.cpp drops an instance out of the rotation after its first
// failed draw ("dropping this card for now"), so a device with two picture
// cards reaches exactly 6 and then stops, sitting at 6/6 - which this limit
// tolerates, since the test below is <= - until Config::kContentRefreshIntervalMs
// re-fetches and re-arms the cards ten minutes later and the next failure is the
// 7th. On device 7 that produced a restart at 620-660s of uptime, every cycle,
// all evening: the period was the content refresh interval, not this check's own
// cadence. Worth knowing before reading a reboot period as a memory curve.
//
// Renamed from kMaxConsecutiveBufferAllocFailures. It never counted buffer
// allocations - see the block above for what the old name cost.
constexpr uint32_t kMaxConsecutiveDrawFailures = 6;

/// How many check-ins in a row may fail before this device restarts itself to
/// get its connection back.
///
/// **Why a restart is the fix and not a workaround.** mbedTLS needs roughly
/// 32KB contiguous for a new TLS session. Once cards have been rendering, the
/// largest 8-bit block settles far below that - 13,812 bytes, measured
/// repeatedly on device 17 - and every handshake then fails with
/// MBEDTLS_ERR_SSL_ALLOC_FAILED. Nothing in this firmware resets the shared
/// client, so the first failure is permanent for the life of the process: the
/// device keeps drawing its cards, and is simultaneously invisible to the
/// server. No telemetry, no check-in, no debug stream, and - the part that
/// matters most - no way to receive a card policy or a firmware update. A
/// device in that state cannot be fixed remotely by anything, including the
/// update that would fix it.
///
/// Across a full evening of this on two devices, a restart recovered it every
/// single time and nothing else ever did. So the honest response is to do
/// deliberately what a person was otherwise doing by hand: reboot.
///
/// Five, because check-in runs on the server's own cadence (60s by default),
/// making this about five minutes of confirmed unreachability. Long enough
/// that an ordinary server deploy, a Kestrel restart or a brief AP glitch
/// passes underneath it - the counter clears on the first success - and short
/// enough that a stuck device is back inside the window where a policy or
/// firmware change can actually reach it.
constexpr uint32_t kMaxConsecutiveCheckInFailures = 5;

/// Consecutive failed check-ins. Cleared by the first success, so this only
/// ever climbs while the device is genuinely unable to reach the server.
uint32_t gConsecutiveCheckInFailures = 0;

/// When this device last restarted itself for unreachability, so it cannot do
/// it again immediately. 0 means "not since boot".
uint32_t gLastUnreachableRestartMs = 0;

/// Whether the "holding off rather than rebooting" explanation has already been
/// said for the current episode. checkUnreachableWatchdog() runs every loop
/// iteration, so without this the message repeats forever - it did, hundreds of
/// times in two minutes on a real device, burying every other line in the remote
/// stream. Cleared on the first successful check-in so a later episode is
/// announced again rather than silently held off.
bool gHoldOffLogged = false;

/// The minimum gap between two unreachability restarts.
///
/// This is the guard against the failure mode this whole mechanism could
/// otherwise become: if the service is genuinely down - or this device's WiFi
/// credentials still work but the server is gone - a restart cannot fix
/// anything, and without a floor here the device would reboot every five
/// minutes forever, which is worse than sitting quietly and retrying. Twenty
/// minutes means a device that is wrong about the cause costs the household
/// three reboots an hour rather than twelve, while a device that is right is
/// back within five minutes.
constexpr uint32_t kMinMsBetweenUnreachableRestarts = 20UL * 60UL * 1000UL;

/// Checked once per loop() iteration, but only actually looks at anything once
/// every kHeapCheckIntervalMs - see the block comments above for the grace
/// period and for why this counts failures rather than free bytes.
void checkHeapHealth() {
  const uint32_t now = millis();
  if (now < kHeapCheckGraceMs) {
    return;
  }
  if (now - lastHeapCheckMs < kHeapCheckIntervalMs) {
    return;
  }
  lastHeapCheckMs = now;

  // Logged on every check whether or not anything is wrong, and both numbers
  // together on purpose. heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) is
  // the real figure - 8BIT because that is the pool a byte-addressable
  // allocation comes from, which on the draw path today means LovyanGFX's own
  // decode scratch (see Display.cpp's releaseDecodeMemory()) rather than any
  // buffer this firmware allocates - and ESP.getMaxAllocHeap() is the one three
  // thresholds were wrongly derived from. Keeping them side by side in the
  // fleet's logs is what makes the gap between them (6,132 against 32,756 at the
  // one instant both were captured on device 17) an observable fact on real
  // hardware rather than a finding that has to be taken on trust, and it is the
  // same reasoning Telemetry.cpp uses for sending freeHeapBytes alongside
  // free8BitBytes instead of replacing it.
  //
  // **The heap figure on this line is context, not the trigger, and the wording
  // below now says so.** It used to read "consecutive file-buffer alloc
  // failures", naming a buffer that has not existed since draws became
  // streaming, sitting immediately after two heap numbers - so the line read as
  // a memory report with a memory trigger. It is a draw report: the count is
  // failed card draws and nothing about the heap decides anything here.
  const size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const uint32_t failures = Display::consecutiveDrawFailures();
  Log::printf(
      "[health] largest 8BIT block=%u (ESP.getMaxAllocHeap says %u at the same instant), "
      "consecutive failed draws=%lu/%lu after %lu ms uptime",
      static_cast<unsigned>(largestBlock), static_cast<unsigned>(ESP.getMaxAllocHeap()),
      static_cast<unsigned long>(failures),
      static_cast<unsigned long>(kMaxConsecutiveDrawFailures),
      static_cast<unsigned long>(now));

  if (failures <= kMaxConsecutiveDrawFailures) {
    return;
  }

  // Past this point the device has demonstrably stopped being able to draw, so
  // the restart is back - the same restart, with the same care, on a trigger
  // that means something this time.
  //
  // The risk being accepted, said out loud: if a restart does not fix it, this
  // becomes a restart every kHeapCheckGraceMs + one interval (about four
  // minutes), which is exactly what the 60,000 threshold did. The difference is
  // what it takes to get there. That loop fired on devices that were drawing
  // their cards perfectly; this one can only fire on a device that has failed
  // more than two cards outright, and such a device is already showing a
  // household nothing. A rebooting device at least retries against a fresh heap
  // and keeps reporting telemetry; and because the counter clears on the first
  // success after the reboot, one that recovers stops restarting immediately.
  Log::printf(
      "[health] %lu consecutive failed card draws (limit %lu) after %lu ms uptime - this "
      "device can no longer draw its cards, restarting to reclaim memory "
      "(largest 8BIT block=%u)",
      static_cast<unsigned long>(failures), static_cast<unsigned long>(kMaxConsecutiveDrawFailures),
      static_cast<unsigned long>(now), static_cast<unsigned>(largestBlock));
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
  // So the next boot reports SOFTWARE_RESET + LOW_HEAP rather than an
  // unexplained software reset. This is the restart that spent a night looking
  // like a crash loop, which is exactly why it should name itself.
  BootDiag::recordRestartIntent(BootDiag::RestartCause::LowHeap);
  // Beside the cause, not instead of it: BootDiag says what the LAST restart
  // was for, this says how many in a row this device has now needed. The next
  // boot reads both - the first to decide whether to be quiet, the second to
  // decide whether it has been quiet for too long. See
  // Identity::consecutiveSelfRestarts() and kMaxSilentSelfRestarts.
  Identity::recordSelfRestart();
  Display::showStatus("Refreshing", "Reclaiming memory - back in a moment");
  // Long enough for both the status message and the flushed log line to be
  // visibly sent before the restart cuts everything off.
  delay(1500);
  esp_restart();
  // Unreachable: the call above never returns.
}

/// Restarts this device when it has demonstrably lost the ability to reach the
/// server, which is the only recovery this firmware has for a poisoned TLS
/// client. See kMaxConsecutiveCheckInFailures for the measurement behind that
/// claim and why it is a fix rather than a workaround.
///
/// Deliberately separate from checkHeapHealth() above even though the two are
/// shaped identically: one fires on a device that cannot draw, the other on a
/// device that cannot be reached, and a device can be in either state while
/// perfectly healthy in the other. Merging them would mean one threshold
/// standing in for two unrelated diagnoses.
void checkUnreachableWatchdog() {
  if (gConsecutiveCheckInFailures < kMaxConsecutiveCheckInFailures) {
    return;
  }

  // WiFi first. If this device is not associated then the server being
  // unreachable is a network fact, not a TLS one, and a restart fixes nothing
  // - it would just reboot repeatedly through an outage that has nothing to do
  // with this firmware. WifiJoin's own reconnect handling owns that case.
  if (WiFi.status() != WL_CONNECTED) {
    Log::printf("[health] %lu check-ins have failed, but WiFi is not associated (status=%d) - "
                "this is a network outage, not a stuck TLS client, so NOT restarting",
                static_cast<unsigned long>(gConsecutiveCheckInFailures),
                static_cast<int>(WiFi.status()));
    return;
  }

  const uint32_t now = millis();

  // Subtraction, not addition, so this stays correct across millis()' 49-day
  // rollover - the same reasoning StackWatch's heartbeat documents. A device
  // that has never restarted for this reason has gLastUnreachableRestartMs 0,
  // and `now - 0` is simply uptime, which is what we want to compare.
  if (gLastUnreachableRestartMs != 0 &&
      (now - gLastUnreachableRestartMs) < kMinMsBetweenUnreachableRestarts) {
    // Logged once per hold-off, not once per loop iteration. This function runs
    // every iteration, so an unconditional line here put hundreds of identical
    // entries into the remote debug stream in a couple of minutes on a real
    // device - the same flood StackWatch::logHighWaterMark was fixed for
    // earlier, reintroduced here by writing the log line without asking how
    // often the surrounding code runs. The stream is the only diagnostic
    // channel a deployed device has, and this was spending all of it saying
    // nothing had changed.
    //
    // Reset when the counter clears on a successful check-in, so the next
    // distinct episode announces itself once.
    if (!gHoldOffLogged) {
      gHoldOffLogged = true;
      Log::printf("[health] %lu check-ins have failed, but this device already restarted for that "
                  "%lu ms ago - holding off (minimum gap %lu ms) rather than entering a reboot "
                  "loop. Silent from here until this clears.",
                  static_cast<unsigned long>(gConsecutiveCheckInFailures),
                  static_cast<unsigned long>(now - gLastUnreachableRestartMs),
                  static_cast<unsigned long>(kMinMsBetweenUnreachableRestarts));
    }
    return;
  }

  Log::printf(
      "[health] %lu consecutive check-in failures (limit %lu) after %lu ms uptime with WiFi "
      "associated - this device can render but cannot be reached, managed or updated, so it is "
      "restarting to get a working TLS session (largest 8BIT block=%u, mbedTLS needs ~32KB "
      "contiguous for a new one)",
      static_cast<unsigned long>(gConsecutiveCheckInFailures),
      static_cast<unsigned long>(kMaxConsecutiveCheckInFailures),
      static_cast<unsigned long>(now),
      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));

  // The line above has to survive the restart, and the remote stream is the
  // only channel a deployed device has - except that this is the one restart
  // where that channel is exactly what is broken. Flushed anyway: it costs
  // nothing on a device that cannot send, and on a device whose failure was
  // asset fetches rather than the log stream it is the whole explanation.
  Log::flushNow();
  AppService::stashTimeForFastReboot();
  BootDiag::recordRestartIntent(BootDiag::RestartCause::Unreachable);
  // Same pairing as checkHeapHealth() above, and this is the watchdog the
  // counter matters most for: an unreachable device that a restart does not fix
  // is precisely the one that would otherwise reboot silently forever with
  // nobody - server or household - ever being told.
  Identity::recordSelfRestart();
  Display::showStatus("Reconnecting", "Restoring the connection - back in a moment");

  // Set before the restart even though this variable does not survive one,
  // because it is what stops a SECOND restart inside this boot if the device
  // somehow reaches the threshold again before restarting.
  //
  // Backing off across reboots is handled elsewhere and has to be: setup()
  // seeds this from BootDiag::lastRestartCause(), so a device that restarts
  // for unreachability and comes back still unreachable starts its next boot
  // with the clock already running rather than at zero. Without that half, this
  // line would be decoration - the restart clears it, and the device would be
  // eligible again five minutes later, forever.
  gLastUnreachableRestartMs = now;

  delay(1500);
  esp_restart();
  // Unreachable: the call above never returns.
}

/// Clears the consecutive-self-restart budget once this boot has lasted long
/// enough to say the restart it followed actually worked.
///
/// The counterpart to Identity::recordSelfRestart() at the two watchdogs above,
/// and the half that keeps the budget from being a one-way ratchet: without it,
/// a device that needed three restarts across a year would narrate every boot
/// it ever had afterwards, which is the noise this whole feature exists to
/// remove, just delayed.
///
/// Checked once per loop() iteration and latched, so this is one comparison in
/// the steady state and at most one flash write per boot - and
/// Identity::clearSelfRestarts() declines even that when there is nothing to
/// clear, which is the ordinary case on a healthy device.
void checkSelfRestartRecovery() {
  if (gSelfRestartsCleared) {
    return;
  }
  if (millis() < kSelfRestartRecoveredUptimeMs) {
    return;
  }
  // Latched before the work, not after, so a clear that somehow fails is not
  // retried on every one of the millions of loop iterations that follow.
  gSelfRestartsCleared = true;

  const uint8_t before = Identity::consecutiveSelfRestarts();
  if (before == 0) {
    return;
  }
  Identity::clearSelfRestarts();
  Log::printf("[health] %lu ms of uptime with no further self-restart - the last one worked, so "
              "the run of %u is cleared and the next one starts its silence budget over",
              static_cast<unsigned long>(kSelfRestartRecoveredUptimeMs),
              static_cast<unsigned>(before));
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
  const AppUpdater::UpdateCheck result = AppUpdater::checkForUpdate();

  if (result == AppUpdater::UpdateCheck::Newer) {
    Log::line("[update] manual check found a newer version");
    Display::showStatus("Updating", "A new version is available");
    // Recorded before handing off, so the next boot can say SOFTWARE_RESET + OTA

    // rather than just "something restarted us" - see BootDiag.h.

    BootDiag::recordRestartIntent(BootDiag::RestartCause::Ota);

    Loader::requestUpdate();
    // Unreachable: the call above never returns.
  }

  // A check that could not happen must not be reported as a check that found
  // nothing. Someone is standing at this device having just pressed the button
  // precisely because they want an update; telling them "Already up to date"
  // when the server was never reached sends them away believing the opposite
  // of the truth. Device 7 did exactly this - it showed "up to date" while
  // running a build four versions behind the fleet, having last reached the
  // server four hours earlier.
  if (result == AppUpdater::UpdateCheck::CheckFailed) {
    Log::line("[update] manual check FAILED - could not reach the server, update state unknown");
    Display::showStatus("Could not check", "The service could not be reached");
    delay(2500);  // Longer than the up-to-date case: this one asks to be read.
    CardManager::redraw();
    return;
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
      // So the next boot reads SOFTWARE_RESET + REPROVISION - see BootDiag.h.

      BootDiag::recordRestartIntent(BootDiag::RestartCause::Reprovision);

      Loader::returnToLoaderForReprovisioning();
      // Unreachable: the call above never returns.
    }
    // Counted, not acted on here - see checkUnreachableWatchdog() for what
    // happens when this keeps happening, and why a restart is the only
    // recovery this firmware has for it.
    ++gConsecutiveCheckInFailures;
    Log::printf("[checkin] failure %lu of %lu before this device restarts to recover its "
                "connection",
                static_cast<unsigned long>(gConsecutiveCheckInFailures),
                static_cast<unsigned long>(kMaxConsecutiveCheckInFailures));

    // Buy back contiguous heap before the next attempt, in case that is what
    // the handshake was short of. A new TLS session needs roughly 32KB
    // contiguous; on a device with no SD card each graphic holds its picture
    // in a RAM buffer for the process lifetime, and two of those leave no
    // such hole anywhere. See Graphic::releaseRamBuffers().
    //
    // Tried here, on the failure path, rather than before every request:
    // pre-emptively freeing would make a healthy card-less device re-fetch
    // its pictures over the network every single check-in, which is the
    // opposite of what it needs. A device that has just failed a check-in is
    // already not fetching anything.
    //
    // It costs nothing at all on a device with a working SD card - those
    // instances hold no RAM buffer - so this is not gated on detecting
    // fallback mode, and does not need to be.
    Graphic::releaseRamBuffers();
    return;
  }

  // Cleared on the first success, so a device that recovers on its own - or
  // that was only ever seeing a brief server blip - never restarts. Same
  // shape as the draw-failure counter the heap watchdog uses.
  if (gConsecutiveCheckInFailures > 0) {
    Log::printf("[checkin] recovered after %lu consecutive failure(s) - restart no longer needed",
                static_cast<unsigned long>(gConsecutiveCheckInFailures));
    gConsecutiveCheckInFailures = 0;
    // So a later episode explains itself once rather than being held off in
    // silence - see gHoldOffLogged.
    gHoldOffLogged = false;
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

  // And the announcements those cards may carry. Unlike the policy, an empty
  // set here IS applied rather than ignored: the response is a complete
  // statement of what should be showing, so "none" has to be able to clear a
  // banner that has stopped being effective or was dismissed elsewhere. See
  // Cards::setAnnouncements().
  Cards::setAnnouncements(result.announcements, result.announcementCount);

  // Not one-shot, unlike updateAvailable below: this reflects the server's
  // current wish on every successful check-in, so remote debug streaming
  // turns on or off in step with an admin's toggle and recovers on its own
  // after a reboot within one check-in interval - see CheckIn.h's and Log.h's
  // own remarks.
  Log::setStreamingEnabled(result.debugStreamRequested);

  // Deliberately AFTER setStreamingEnabled and after applyPolicy: an admin who
  // has just switched streaming on gets a full picture of every active
  // provider on this very check-in rather than having to wait for the next
  // one, and the statuses reported are the ones for the policy now in force.
  //
  // This re-states each active card's fetch outcome whether or not it changed.
  // The per-module logging is change-only by design, which means a device
  // parked in a steady refused state emits nothing about it - the exact case
  // someone watching a live stream is most likely trying to diagnose. Costs
  // nothing when streaming is off; see Cards::logProviderStatuses().
  Cards::logProviderStatuses();

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
    // Recorded before handing off, so the next boot can say SOFTWARE_RESET + OTA

    // rather than just "something restarted us" - see BootDiag.h.

    BootDiag::recordRestartIntent(BootDiag::RestartCause::Ota);

    Loader::requestUpdate();
    // Unreachable: the call above never returns.
  }

  // There used to be an Http::releaseTlsSession() call here, and removing it is
  // deliberate.
  //
  // The measurement behind it was real: releasing the session returned 41,312
  // bytes and moved the largest contiguous 8BIT block from 6,132 to 36,852, and
  // an identical 10,568-byte allocation that had just failed then succeeded. It
  // proved that peak concurrent use, not fragmentation, was the constraint.
  //
  // But it was the wrong end of the problem, and shipping it took two devices
  // off the network. It attacked the symptom - too little contiguous memory at
  // draw time - by evicting mbedTLS, when the cause was the draw path taking
  // the memory in the first place. Now that draws stream from SD and take no
  // per-draw block at all (see Display.cpp's drawImageFromSd), there is nothing
  // to make room for, and tearing down a working TLS session every cycle buys
  // nothing while costing a full handshake.
  //
  // If TLS lifetime is ever worth revisiting, calling stop() on the shared
  // process-lifetime NetworkClientSecure is not the way: the debug log stream
  // and telemetry ride the same client, so a client left unusable takes the
  // device's own diagnostic channel down with it. A short-lived per-request
  // client would be the safe shape.
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

  // Right here, before anything else can obscure it: why the last restart
  // happened, in two parts - how the chip reset, and what this firmware was
  // trying to achieve when it asked for it. Until this existed, an unexpected
  // reboot and a deliberate one were indistinguishable from the server, where
  // the only evidence was the boot counter going up. See BootDiag.h.
  BootDiag::logResetReason();

  // If the previous restart was this device restarting itself for
  // unreachability, start the backoff clock already running rather than at
  // zero. Without this the guard is ineffective across reboots - the variable
  // holding "when did I last restart" does not survive a restart, so a device
  // facing something a reboot cannot fix (the service genuinely down, DNS
  // moved, credentials valid but the host gone) would restart every five
  // minutes indefinitely. Seeded this way it gets one restart, then waits
  // kMinMsBetweenUnreachableRestarts before trying that again, whether or not
  // a reboot happened in between.
  if (BootDiag::lastRestartCause() == BootDiag::RestartCause::Unreachable) {
    gLastUnreachableRestartMs = millis();
    Log::printf("[health] last restart was for unreachability - the connection watchdog will hold "
                "off for %lu ms rather than restart again immediately",
                static_cast<unsigned long>(kMinMsBetweenUnreachableRestarts));
  }

  Display::begin();

  // Drawn on every boot, quiet ones included, and deliberately BEFORE
  // decideBootNarration() exists to suppress anything. See the "What a boot is
  // allowed to say on the glass" block above: a therapeutic reboot gets one
  // screen rather than no screen, this is the screen that covers the gap while
  // SD mounts and the splash decodes, and it names no network, no clock and no
  // server - it is not part of the handshake chatter being removed.
  Display::showStatus("Starting", "");

  // Already ahead of the splash, and now load-bearing rather than incidental:
  // decideBootNarration() below reads Identity::consecutiveSelfRestarts(), so
  // prefs.begin() has to have happened by then. Anything that moves this line
  // later has to move that decision later too.
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

  // Whether the logo actually reached the screen. When it did, every status
  // screen for the rest of this boot is skipped so it stays up for the whole of
  // WiFi join and time sync, rather than being painted over a moment after
  // appearing - the branded boot this feature exists for, and what the splash
  // was always meant to do: the logo first, the network connecting underneath
  // it.
  //
  // **That was already the design and it did not work, which is half of why
  // this file changed.** The skip used to be two `if (!splashOnScreen)` checks
  // around App.ino's own "Checking the time" and "Loading" calls - and WifiJoin
  // draws two status screens of its own that neither check could ever cover.
  // They land a few hundred milliseconds after the splash, so the logo was a
  // single frame followed by "Looking for known networks", on every device, for
  // the whole life of the feature. The README's claim that the logo stays up
  // through the WiFi join was describing an intention, not the binary. The skip
  // is now one decision (decideBootNarration below) pushed into every module
  // that draws during boot, which is why it reaches WifiJoin at all.
  //
  // A device with no splash - no card, nothing configured, or a decode that
  // failed - falls back to those status screens exactly as before, unless this
  // boot is a quiet one for its own reasons. Silence on a blank panel while
  // WiFi retries is a worse boot than a plain status line. Failure screens (see
  // ensureWifiConnected) are never suppressed on any path: a household that
  // needs to hold BOOT to fix its WiFi has to be told so.
  const bool splashOnScreen = Assets::showBootSplash();
  if (splashOnScreen) {
    // Read only by holdSplash() at the bottom of setup(), which is what stops
    // the first card replacing the logo before anyone has seen it.
    gSplashDrawnAtMs = millis();
  }

  // Everything below this line that draws boot progress goes through
  // bootStatus() or WifiJoin's own gate, and this is where both are decided.
  // See the "What a boot is allowed to say on the glass" block near the top of
  // this file for the reasoning; it is the substance of this change.
  decideBootNarration(splashOnScreen);

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
    // So the next boot reads SOFTWARE_RESET + REPROVISION - see BootDiag.h.

    BootDiag::recordRestartIntent(BootDiag::RestartCause::Reprovision);

    Loader::returnToLoaderForReprovisioning();
    // Unreachable: the call above never returns.
  }
  // Reached when BOOT was never touched (a no-op) or when it was held and let
  // go before the three seconds - see restoreHeldBootScreen() for why a quiet
  // boot has to tidy that up explicitly now that nothing else will.
  restoreHeldBootScreen(splashOnScreen);

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
    // Through bootStatus() rather than a local `if (!splashOnScreen)`: the
    // splash is no longer the only reason a boot stays quiet, and a second copy
    // of that rule here is how the WifiJoin screens came to be missed in the
    // first place.
    bootStatus("Checking the time", "Needed before a secure connection");
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
  // "Loading" is skipped on a quiet boot, for the same reason the time screen
  // above is: CardManager::poll() holds whatever is on screen until a real
  // policy arrives (see its own gPolicyEverApplied remarks), so leaving the
  // splash - or the single "Starting" screen a splash-less quiet boot holds -
  // there means it stays put right up until the first real card replaces it,
  // instead of a blank "Loading" filling that gap.
  bootStatus("Loading", "");

  // The last thing before the rotation takes the screen, because
  // CardManager::begin() fetches and draws one card immediately - it is the
  // thing that replaces the splash, and on a fast boot it can do so within a
  // second or two of the logo appearing. See kMinSplashOnScreenMs for why that
  // is the second half of "the splash is on screen for a split second" and why
  // this wait is usually zero.
  holdSplash();

  // Narration goes back on before loop() ever runs, and it has to: every
  // showStatus() beyond this point is a running device reporting something
  // that is happening NOW to somebody who may well be looking at it - an
  // update installing, a reconnect, a WiFi drop mid-rotation. None of that is
  // boot progress, and leaving the gate shut would silence it for the whole
  // uptime rather than for the boot.
  gNarrateBoot = true;
  WifiJoin::setProgressVisible(true);

  CardManager::begin();
}

void loop() {
  // Captured before anything blocking runs - see the instrumentation at the
  // bottom of this function, which reports iterations long enough that a tap
  // could have been dropped inside them.
  const uint32_t iterationStartMs = millis();

  ensureWifiConnected();

  // Independent of every other timer in this loop, and checked early - see
  // checkHeapHealth()'s own remarks for why this restarts the App directly
  // rather than routing through Loader.cpp.
  checkHeapHealth();

  // Checked here rather than inside performCheckIn() so the decision to
  // restart is never taken while a request is part-way through, and so it is
  // visible in the same place as the heap watchdog it is a sibling of. It
  // costs a comparison on every iteration and does nothing until five
  // check-ins in a row have failed.
  checkUnreachableWatchdog();

  // Beside the two watchdogs because it is their counterpart: they count this
  // device's self-restarts, and this is the one thing that ever clears the
  // count. Costs one comparison per iteration and fires at most once per boot.
  checkSelfRestartRecovery();

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
  // After the render path, which is the other candidate for the deepest frame
  // in this loop - a card draw goes down into LovyanGFX's decoder.
  StackWatch::logHighWaterMark("after render");

  const uint32_t now = millis();

  if (now - lastCheckInMs >= checkInIntervalMs) {
    lastCheckInMs = now;
    performCheckIn();
    // The suspected deepest path: a TLS handshake, a JSON parse, and a ~4KB
    // CheckIn::Result, all in one call chain. StackWatch keeps the worst-ever
    // figure, so this is where it is most likely to be set - and measuring it
    // is the whole point, since every previous claim about stack headroom in
    // this file was arithmetic on the size of local objects rather than an
    // observation. See StackWatch.h.
    StackWatch::logHighWaterMark("after check-in");
  }

  // Belt-and-braces fallback only: performCheckIn() above is the fast path
  // that actually reaches an admin's "Force update" button or a fresh
  // version within one checkInIntervalMs. This independent, much slower
  // timer exists purely so an update is never permanently missed if
  // check-in itself were ever broken.
  if (now - lastUpdateCheckMs >= Config::kUpdateCheckIntervalMs) {
    lastUpdateCheckMs = now;
    // Only Newer acts. CheckFailed deliberately does nothing beyond what
    // checkForUpdate() already logged: this is an unattended background timer,
    // so a transient network failure should quietly wait for the next tick
    // rather than put an error on a wall display nobody asked. That is the
    // opposite of the BOOT-button path above, where a person is waiting for an
    // answer and silence is the wrong response.
    if (AppUpdater::checkForUpdate() == AppUpdater::UpdateCheck::Newer) {
      Log::line("[update] fallback timer found a newer version - rebooting into CAL");
      Display::showStatus("Updating", "A new version is available");
      // Recorded before handing off, so the next boot can say SOFTWARE_RESET + OTA

      // rather than just "something restarted us" - see BootDiag.h.

      BootDiag::recordRestartIntent(BootDiag::RestartCause::Ota);

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

  // Still ~50ms of pacing, but spent sampling touch rather than asleep.
  //
  // The pacing itself was never the problem. Everything else in this loop is
  // gated on its own millis() comparison and does not care how often it is
  // asked; touch is the one thing that does, because a finger is on the glass
  // for a fraction of a second. What made advance/rewind feel unresponsive is
  // that a tap was only ever seen if it happened to overlap the single
  // Touch::poll() inside CardManager::poll() - one sample per iteration - and
  // this loop's iterations are not evenly spaced. performCheckIn() above is a
  // synchronous TLS handshake plus request and response, and the card refresh
  // inside CardManager::poll() is a synchronous HTTPS fetch, so the real gap
  // between two touch samples is sometimes seconds. Taps landing in those
  // stretches were dropped silently, which is the worst possible failure for a
  // button whose whole design is that the person gets no confirmation and so
  // has no way to tell a missed press from a slow one.
  //
  // Sampling every 5ms across the wait fixes the between-operations half of
  // that outright. It does NOT fix sampling during a blocking call - that
  // needs the network work off this path, which is a much bigger change than
  // this - so the instrumentation below exists to say how much of the
  // remaining problem that actually is, measured rather than assumed.
  constexpr uint32_t kLoopPacingMs = 50;
  constexpr uint32_t kTouchSampleIntervalMs = 5;
  const uint32_t pacingStartMs = millis();
  while ((millis() - pacingStartMs) < kLoopPacingMs) {
    CardManager::pollTouch();
    delay(kTouchSampleIntervalMs);
  }

  // How long this whole iteration took, and therefore how long touch went
  // unsampled at the worst point in it. Logged only when it is bad enough to
  // matter - an ordinary iteration is the ~50ms above and saying so every
  // 50ms would drown the stream it is written to.
  //
  // kUnresponsiveIterationMs is set just above a normal iteration rather than
  // at some round number, so anything logged here is genuinely a stretch where
  // a tap could have been lost. Whoever picks up the reported unresponsiveness
  // next should read these lines first: if they are rare, the 5ms sampling
  // above was the whole fix, and if they are common, the fix is to get the
  // request path out of loop().
  constexpr uint32_t kUnresponsiveIterationMs = 250;
  const uint32_t iterationMs = millis() - iterationStartMs;
  if (iterationMs >= kUnresponsiveIterationMs) {
    Log::printf("[loop] iteration took %lu ms - touch was unsampled for most of it",
                static_cast<unsigned long>(iterationMs));
  }
}
