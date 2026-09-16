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

/// Millivolts at or above which GPIO35 counts as HIGH.
///
/// 1500, sitting in the empty middle of a gap the fleet measured for us rather
/// than in the middle of the supply rail. Device 17's AM312 reads 3155 mV when
/// triggered; four devices with nothing attached to that pin read 142, 147, 152
/// and 205 mV, and 17 itself reads 142 when idle. So the real signal is a factor
/// of fifteen away from the noise floor and any threshold from roughly 400 to
/// 3000 would separate them - 1500 is chosen for being far from BOTH ends, so
/// neither a weak sensor nor a noisier pin can reach across it.
///
/// This is a threshold and not a comparison to 3.3V because the reading is taken
/// at 11dB attenuation, where the ESP32's ADC saturates well below the rail and
/// cannot resolve below about 140 mV. 3155 is what "3.3V" actually looks like
/// through this path, and 142 is what 0V looks like - which is also why an absent
/// sensor and an idle one are indistinguishable here. See Motion.h.
constexpr uint32_t kHighThresholdMillivolts = 1500;

/// Minimum gap between ADC reads of the sensor pin.
///
/// service() is called every loop iteration, and an analogReadMilliVolts() on
/// every one of those would be pure waste: the debounce below already requires
/// two HIGH readings kDebounceMs (150ms) apart, so sampling faster than that
/// cannot make a detection happen any sooner. 50ms gives three samples inside
/// every debounce window - enough that the pair is always found promptly - at a
/// fraction of the reads.
///
/// The reason to care is loop latency rather than CPU. This project has a filed,
/// reproduced defect about card advance/rewind taps being dropped when an
/// iteration runs long, and the fade in this very module was written
/// non-blocking for the same reason. Adding an unbounded per-iteration hardware
/// read to the hot loop to service a sensor that changes on human timescales
/// would be trading a real thing for nothing.
constexpr uint32_t kSampleIntervalMs = 50;

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

/// Fade speed, in percentage points per second (BL 03).
///
/// TIME-BASED, NOT PER-ITERATION, and that is the fix rather than the style. The
/// first version moved a fixed 5% on every loop iteration, which tied the fade's
/// duration to the loop's speed - and this loop is not steady: measured on device
/// 17 on 2026-09-16, iterations ran 360ms in the quiet case and 3.7 to 6.0
/// SECONDS during the check-in and telemetry pair. So a 100-to-25 fade took
/// anywhere from three seconds to over a minute depending on where it started,
/// and a single step could hang for six seconds mid-fade, which reads as a stuck
/// screen rather than a dimming one.
///
/// 40 points per second puts a full 100-to-0 sweep at 2.5 seconds and a typical
/// 100-to-25 at under two, regardless of what the loop is doing.
///
/// Still non-blocking, which is the requirement this constant serves: there is no
/// loop here that could hold the main loop, and therefore no way for a brightness
/// change to cost a dropped tap. The App's own instrumentation already reports
/// iterations long enough to lose a touch; a fade must never be one of them.
constexpr uint32_t kFadePercentPerSecond = 40;

Policy gPolicy;
BacklightState gState = BacklightState::Fallback;
FaultCode gFault = FaultCode::None;

bool gBegun = false;
uint32_t gBootMs = 0;

/// Debounce state: when the first HIGH of a candidate pair was seen, 0 for none.
uint32_t gFirstHighMs = 0;
/// When the ADC was last read, so kSampleIntervalMs can be honoured. 0 = never.
uint32_t gLastSampleMs = 0;
/// The last reading taken, reused on the iterations that skip the ADC so the
/// stuck-high and debounce logic see a continuous signal rather than gaps.
uint32_t gLastMillivolts = 0;
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

/// When stepFade last moved the brightness, for the time-based fade. 0 = never.
uint32_t gLastFadeMs = 0;

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

/// One fade step toward the target, sized by elapsed time. Deliberately the only
/// place brightness is written, so no path can set a level the state machine does
/// not know about.
void stepFade() {
  const uint32_t now = millis();
  if (gLastFadeMs == 0) {
    gLastFadeMs = now;
    return;
  }

  const uint8_t current = Display::brightness();
  if (current == gTargetPercent) {
    // Keep the clock current while settled, so arriving at a new target does not
    // jump by however long the display sat still beforehand.
    gLastFadeMs = now;
    return;
  }

  const uint32_t step = ((now - gLastFadeMs) * kFadePercentPerSecond) / 1000UL;
  if (step == 0) {
    // NOT advancing the clock is the point: the remainder accumulates instead of
    // being discarded. Rounded away every call, a fast loop would move 0% forever
    // and the display would never reach its target at all.
    return;
  }
  gLastFadeMs = now;

  const uint32_t distance = current < gTargetPercent
                                ? static_cast<uint32_t>(gTargetPercent - current)
                                : static_cast<uint32_t>(current - gTargetPercent);
  const uint8_t moved = static_cast<uint8_t>(step < distance ? step : distance);
  Display::setBrightness(current < gTargetPercent
                             ? static_cast<uint8_t>(current + moved)
                             : static_cast<uint8_t>(current - moved));
}

/// True when a confirmed motion event has just occurred.
bool sampleSensor() {
  const uint32_t now = millis();

  if (now - gBootMs < kStabilizeMs) {
    return false;
  }

  // READ THROUGH THE ADC, NOT digitalRead(), AND THIS IS NOT A STYLE CHOICE.
  //
  // The first version of this file used digitalRead(35) and never once saw the
  // PIR on device 17 - which has a working AM312, proven by PowerProbe's own
  // telemetry reporting 3155 mV on motion against a 142 mV idle floor, at the
  // same times this module was reporting nothing at all.
  //
  // The cause is that GPIO35 has two claimants. PowerProbe::begin() calls
  // analogReadMilliVolts(35) - and does it again on every telemetry report -
  // which routes that pad to ADC1_CH7 and takes the digital input path away with
  // it. PowerProbe::begin() runs at App.ino:2193, a hundred lines AFTER
  // Motion::begin()'s pinMode at 2089, so the ADC claim always lands last and
  // digitalRead() is reading a pad that is no longer wired to it.
  //
  // Two modules sharing one pin, where one silently disables the other, is the
  // kind of bug that cannot be found by reading either module. It cost a night of
  // black screen on a bench device and would have been invisible in a household -
  // the panel would simply never have woken.
  //
  // So this now reads the pin the ONE way that is demonstrated to work on this
  // hardware, which also ends the conflict rather than papering over it: there is
  // a single access mode for GPIO35 in this firmware, and it is the ADC. A
  // consequence worth having is that Motion and PowerProbe can no longer disagree
  // about the same pin, because they are now reading the same number.
  // Throttled to kSampleIntervalMs - see that constant. Between reads the last
  // value is reused rather than treating the gap as LOW, which would shred the
  // debounce pair and the stuck-high timer both.
  if (gLastSampleMs == 0 || now - gLastSampleMs >= kSampleIntervalMs) {
    gLastMillivolts = analogReadMilliVolts(kPin);
    gLastSampleMs = now;
  }
  const bool high = gLastMillivolts >= kHighThresholdMillivolts;

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

  // NO pinMode, and its absence is the fix rather than an omission.
  //
  // This used to call pinMode(kPin, INPUT) to set up a digital read. That read
  // never worked, because PowerProbe::begin() runs later in setup() and claims
  // the same pad for ADC1_CH7 - see sampleSensor() for the full account. Setting
  // a digital mode here would now be worse than useless: it would be a line of
  // code asserting an access mode this module no longer uses, for a pin another
  // module reconfigures a hundred lines later anyway.
  //
  // analogReadMilliVolts() needs no per-pin setup. Attenuation is the one thing
  // that does need setting and PowerProbe::begin() already does it globally
  // (analogSetAttenuation(ADC_11db)), which is also why the figures in
  // kHighThresholdMillivolts are the figures this module will actually see.
  //
  // There is still no pull resistor and there cannot be: GPIO35 is in the
  // input-only 34-39 range, which has none on this part. That is precisely why
  // absent and idle read the same - see Motion.h.
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
  } else if (idleMs < activeMs + dimMs || !gEverDetected) {
    // THE FLOOR IS DIM, NOT OFF, UNTIL THIS DEVICE HAS SEEN REAL MOTION ONCE.
    //
    // Blacking out a panel is only defensible on evidence that something will
    // turn it back on. Before the first confirmed event this module has no such
    // evidence: an operator declared a sensor, and a declaration is a belief
    // about hardware, not an observation of it. The whole design already refuses
    // to let the SERVER assert hardware it cannot see (Motion.h, and
    // MOTION_AWARE_DISPLAY_DESIGN.md's governing rule) - this applies the same
    // rule to the actuator instead of the reporter, which the first version of
    // this file did not do.
    //
    // Learned the expensive way on 2026-09-16. Device 17 ran with a declared
    // sensor that had never fired, went to 0% on the dim timeout, and sat black
    // for eight hours; touch could not wake it because that wiring was missing
    // too. Either bug alone would have been recoverable. Together they produced a
    // panel with no way back except a power cycle, which is exactly what a
    // remotely-deployed device does not have available.
    //
    // Dim is the honest middle: it still saves most of the backlight, it is
    // visibly alive rather than apparently dead, and it cannot strand anybody. A
    // device that confirms motion once this boot gets the full behaviour from
    // that moment on, and the transition is logged so the unlock is visible on
    // the stream rather than inferred.
    const char* why = idleMs < activeMs + dimMs
                          ? "active timeout expired"
                          : "dim timeout expired, but no motion has ever been confirmed on this "
                            "device - holding dim rather than going dark, because nothing has "
                            "yet proved anything can wake it";
    enterState(BacklightState::Dim, gPolicy.dimPercent, why);
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
