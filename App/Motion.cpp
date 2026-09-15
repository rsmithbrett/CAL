#include "Motion.h"

#include "Display.h"
#include "Log.h"

namespace Motion {
namespace {

/// The AM312 OUT line. Input-only at the silicon level on this part, which is
/// exactly what a sensor line wants: nothing in this firmware can drive it by
/// mistake. Broken out on the P3 header as 3.3V / IO35 / GND.
constexpr uint8_t kPin = 35;

/// Settling time after power-up before readings count (MOT 01). An AM312 needs
/// its own warm-up and reports spuriously during it, so a device that booted
/// because somebody walked past would otherwise wake on its own noise.
constexpr uint32_t kStabilizeMs = 30000;

/// Two HIGH samples this far apart to confirm an event (MOT 02). Rejects
/// electrical noise without needing a timer or an interrupt - the loop already
/// turns far faster than this.
constexpr uint32_t kDebounceMs = 150;

/// A session continues while motion keeps arriving, and ends this long after the
/// last confirmed event (MOT 04). This is what stops an AM312's repeat-triggering
/// from counting one visit as forty (MOT 06).
constexpr uint32_t kSessionGapMs = 5UL * 60UL * 1000UL;

/// Held HIGH longer than this and the sensor is treated as suspect.
///
/// TEN MINUTES, not seconds, and the figure comes from observation rather than
/// taste: device 17's sensor read HIGH across consecutive telemetry reports while
/// somebody worked at the bench beside it. A room can legitimately hold a PIR
/// HIGH for a long time. The cost of getting this too short is declaring a
/// working sensor faulty and reverting a household's display to fallback for no
/// reason, so it is set well beyond plausible occupancy rather than close to it.
constexpr uint32_t kStuckHighMs = 10UL * 60UL * 1000UL;

/// Fade step, applied one step per loop iteration (BL 03). Non-blocking by
/// construction - there is no loop here that could hold the main loop, and
/// therefore no way for a brightness change to cost a dropped tap. The App's own
/// instrumentation already reports iterations long enough to lose a touch, and a
/// fade must never be one of them.
constexpr uint8_t kFadeStepPercent = 5;

Policy gPolicy;
BacklightState gState = BacklightState::Fallback;
FaultCode gFault = FaultCode::None;

bool gBegun = false;
uint32_t gBootMs = 0;

/// Debounce state: when the first HIGH of a candidate pair was seen, 0 for none.
uint32_t gFirstHighMs = 0;
/// When the pin was first seen continuously HIGH, for the stuck check. Cleared on
/// any LOW reading.
uint32_t gHighSinceMs = 0;

uint32_t gLastEventMs = 0;
bool gEverDetected = false;
uint32_t gEventsSinceReport = 0;
/// Set by touch as well as motion - the inactivity clock is about a person being
/// there, and a finger is better evidence of that than a PIR (BL 04).
uint32_t gLastActivityMs = 0;

uint8_t gTargetPercent = 100;

uint8_t fallbackPercent() {
  // An unrecognised mode reads as legacyAlwaysOn, which is exactly today's
  // behaviour. A typo in a config row must not decide how a screen behaves
  // (CAP 04, CFG 03) - and the server clamps before sending, so this is the
  // second of two guards rather than the only one.
  if (gPolicy.fallbackMode == "alwaysDim" || gPolicy.fallbackMode == "AlwaysDim") {
    return gPolicy.dimPercent;
  }
  return 100;
}

void enterState(BacklightState next, uint8_t percent, const char* why) {
  if (gState == next && gTargetPercent == percent) {
    return;
  }
  gState = next;
  gTargetPercent = percent;
  Log::printf("[motion] backlight -> %s at %u%% (%s)",
              next == BacklightState::Active   ? "active"
              : next == BacklightState::Dim    ? "dim"
              : next == BacklightState::Off    ? "off"
                                               : "fallback",
              static_cast<unsigned>(percent), why);
}

/// One fade step toward the target. Deliberately the only place brightness is
/// written, so no path can set a level the state machine does not know about.
void stepFade() {
  const uint8_t current = Display::brightness();
  if (current == gTargetPercent) {
    return;
  }
  if (current < gTargetPercent) {
    const uint8_t next = (gTargetPercent - current) <= kFadeStepPercent
                             ? gTargetPercent
                             : static_cast<uint8_t>(current + kFadeStepPercent);
    Display::setBrightness(next);
    return;
  }
  const uint8_t next = (current - gTargetPercent) <= kFadeStepPercent
                           ? gTargetPercent
                           : static_cast<uint8_t>(current - kFadeStepPercent);
  Display::setBrightness(next);
}

/// True when a confirmed motion event has just occurred.
bool sampleSensor() {
  const uint32_t now = millis();

  if (now - gBootMs < kStabilizeMs) {
    return false;
  }

  const bool high = digitalRead(kPin) == HIGH;

  if (!high) {
    gFirstHighMs = 0;
    gHighSinceMs = 0;
    // A LOW reading clears a stuck fault: whatever was holding the line has let
    // go, so the sensor is behaving again. Recovering without a reboot matters
    // because nobody is going to power-cycle a panel in a lobby to clear a
    // diagnostic.
    if (gFault == FaultCode::StuckHigh) {
      Log::line("[motion] sensor released - clearing the stuck-high fault");
      gFault = FaultCode::None;
    }
    return false;
  }

  if (gHighSinceMs == 0) {
    gHighSinceMs = now;
  } else if (gFault != FaultCode::StuckHigh && now - gHighSinceMs >= kStuckHighMs) {
    // Suspect, not broken. Readings are disregarded and the display reverts to
    // fallback, which is the only outcome that cannot leave a panel stuck bright
    // forever (AC 10). Everything else about the device keeps working (CAP 03).
    Log::printf("[motion] sensor has read HIGH continuously for %lu ms - treating it as "
                "stuck and reverting to fallback. Cards, check-in and telemetry are "
                "unaffected; a LOW reading will clear this by itself",
                static_cast<unsigned long>(now - gHighSinceMs));
    gFault = FaultCode::StuckHigh;
  }

  if (gFault == FaultCode::StuckHigh) {
    return false;
  }

  if (gFirstHighMs == 0) {
    gFirstHighMs = now;
    return false;
  }
  if (now - gFirstHighMs < kDebounceMs) {
    return false;
  }

  gFirstHighMs = now;  // stay armed for the next pair rather than latching once

  const bool newSession = gLastEventMs == 0 || (now - gLastEventMs) >= kSessionGapMs;
  gLastEventMs = now;

  if (newSession) {
    // Counted per SESSION, not per HIGH edge. An AM312 retriggers for as long as
    // somebody is in the room, so counting edges would report a person standing
    // still as a crowd (MOT 03, MOT 06).
    gEventsSinceReport++;
  }

  if (!gEverDetected) {
    gEverDetected = true;
    // The one thing this module can say honestly about hardware. Logged loudly
    // because it is also the moment the operator's declaration stops mattering
    // for this device - from here on its own report governs.
    Log::line("[motion] first confirmed motion this boot - something is driving GPIO35, "
              "so this device can now report Present for itself");
  }
  return true;
}

}  // namespace

void begin() {
  gBootMs = millis();
  gLastActivityMs = gBootMs;

  // No pull configured, and none is available: GPIO35 is in the input-only
  // 34-39 range, which has no internal pull resistors on this part. That is
  // precisely why absent and idle read the same - see Motion.h.
  pinMode(kPin, INPUT);
  gBegun = true;

  Log::printf("[motion] watching GPIO35, stabilising for %lu ms before any reading counts. "
              "No sensor fitted is indistinguishable from an idle one, so nothing is "
              "concluded from silence",
              static_cast<unsigned long>(kStabilizeMs));
}

void applyPolicy(const Policy& policy) {
  const bool wasEnabled = gPolicy.present && gPolicy.enabled;
  gPolicy = policy;

  if (!policy.present) {
    // "The server sent no policy" means this device is not one the server
    // believes has a sensor. Revert to fallback - NOT to darkness. Same posture
    // as CardManager's applyPolicy: an absent policy is never an instruction to
    // show nothing.
    enterState(BacklightState::Fallback, fallbackPercent(), "no policy in force");
    return;
  }

  if (!policy.enabled) {
    enterState(BacklightState::Fallback, fallbackPercent(), "policy says motion is not enabled");
    return;
  }

  if (!wasEnabled) {
    Log::printf("[motion] policy applied: active %u%% after %lus, dim %u%% after %lus, "
                "fallback '%s'",
                static_cast<unsigned>(policy.activePercent),
                static_cast<unsigned long>(policy.activeTimeoutSeconds),
                static_cast<unsigned>(policy.dimPercent),
                static_cast<unsigned long>(policy.dimTimeoutSeconds),
                policy.fallbackMode.c_str());
    // Start awake. A device that has just been told it may dim should not dim
    // before anybody has had a chance to be seen by it.
    gLastActivityMs = millis();
    enterState(BacklightState::Active, policy.activePercent, "policy just enabled");
  }
}

void service() {
  if (!gBegun) {
    return;
  }

  const bool detected = sampleSensor();
  if (detected) {
    gLastActivityMs = millis();
  }

  // Not enabled, or no policy, or a suspect sensor: hold fallback and do nothing
  // else. Sensor sampling above still runs (MOT 05) so the stuck-high check keeps
  // working and a first real detection is still noticed.
  if (!gPolicy.present || !gPolicy.enabled || gFault == FaultCode::StuckHigh) {
    enterState(BacklightState::Fallback, fallbackPercent(), "motion not in force");
    stepFade();
    return;
  }

  const uint32_t idleMs = millis() - gLastActivityMs;
  const uint32_t activeMs = gPolicy.activeTimeoutSeconds * 1000UL;
  const uint32_t dimMs = gPolicy.dimTimeoutSeconds * 1000UL;

  if (idleMs < activeMs) {
    enterState(BacklightState::Active, gPolicy.activePercent, "activity");
  } else if (idleMs < activeMs + dimMs) {
    enterState(BacklightState::Dim, gPolicy.dimPercent, "active timeout expired");
  } else {
    // Zero is the BACKLIGHT only. Check-in, content fetch, WiFi and card rotation
    // all continue - BL 05, and the reason this is not a sleep mode.
    enterState(BacklightState::Off, 0, "dim timeout expired");
  }

  stepFade();
}

bool noteTouchAndShouldSwallow() {
  gLastActivityMs = millis();

  // BL 07: the first contact on a panel that is off wakes it and is NOT delivered
  // to the control underneath. A household cannot aim at a button they cannot
  // see, so treating that tap as a press would fire whatever happened to be on
  // screen - an email, a phone call - chosen at random by the rotation.
  const bool wasDark = gState == BacklightState::Off || Display::brightness() == 0;

  if (gPolicy.present && gPolicy.enabled) {
    enterState(BacklightState::Active, gPolicy.activePercent, "touch");
  }

  if (wasDark) {
    Log::line("[motion] touch woke the display - this tap is consumed as the wake and is "
              "deliberately not delivered to the card underneath it");
  }
  return wasDark;
}

bool hasEverDetectedMotion() { return gEverDetected; }

String capabilityToReport() {
  if (gFault == FaultCode::StuckHigh) {
    return "Faulted";
  }
  if (gEverDetected) {
    return "Present";
  }
  // SILENCE, DELIBERATELY. Not "Absent" and not "Unknown": an empty string leaves
  // the server's record and the operator's declaration exactly as they are, which
  // is the only honest thing to send when an idle sensor and a missing one are
  // electrically identical (CAP 05).
  return String();
}

BacklightState state() { return gState; }
FaultCode fault() { return gFault; }

uint32_t eventsSinceReport() { return gEventsSinceReport; }

int32_t lastDetectedAgeSeconds() {
  if (gLastEventMs == 0) {
    return -1;  // never, which the caller sends as null rather than as a zero age
  }
  return static_cast<int32_t>((millis() - gLastEventMs) / 1000UL);
}

void clearReportedCounters() { gEventsSinceReport = 0; }

String operatingState() {
  if (!gPolicy.present) {
    return "fallback";
  }
  return gPolicy.enabled && gFault != FaultCode::StuckHigh ? "enabled" : "disabled";
}

}  // namespace Motion
