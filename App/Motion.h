#pragma once

#include <Arduino.h>

/// Motion-driven backlight control: an optional AM312 PIR on GPIO35 and the
/// brightness state machine it feeds.
///
/// **THIS MODULE NEVER DECIDES THAT A SENSOR EXISTS.** It cannot, and the reason
/// is measured rather than argued. On 2026-09-15 the fleet's own PowerProbe
/// telemetry was sampled on device 17, which has an AM312 wired to GPIO35:
///
///     19:16:01   3155 mV    motion
///     19:17:04    142 mV    idle
///     19:18:01   3155 mV    motion
///     19:19:02    142 mV    idle, and for four minutes after
///
/// Four devices with nothing attached to that pin read 142, 147, 152 and 205 mV.
/// So **an idle sensor and an absent sensor produce the same reading** - the
/// ESP32's ADC at this attenuation cannot resolve below about 140 mV, so 0 V and
/// a floating input both clamp to the same floor. That is a property of the ADC,
/// not of the sensor, which is also why a pull-DOWN resistor would not help;
/// only a pull-up would, by moving "absent" to the top of the range instead.
///
/// The consequence shapes this whole file: **Present can be reported honestly,
/// Absent can never be.** A confirmed HIGH means something is actively driving
/// the pin. Silence means nothing whatsoever - empty room, no sensor, cut wire,
/// all identical. So this module reports Present once it has seen real motion and
/// otherwise reports nothing at all, leaving the question to the operator
/// declaration the server holds. See MOTION_AWARE_DISPLAY_DESIGN.md.
///
/// **What turns the feature on is the policy, not the pin.** The server sends a
/// motionPolicy only to a device it believes has a sensor; receiving one with
/// enabled=true is the instruction to start watching GPIO35. A device that
/// receives no policy does exactly what it does today, which is `AC 01`/`AC 02`.
namespace Motion {

/// What the display is doing right now.
enum class BacklightState : uint8_t {
  /// No policy in force - the sensor is absent, undeclared, disabled or faulted.
  /// Brightness is left wherever the fallback mode put it, which by default is
  /// exactly today's always-on behaviour.
  Fallback = 0,
  Active,
  Dim,
  Off,
};

/// A bounded diagnostic, matching the server's SensorFaultCode enum. Stuck-LOW is
/// deliberately absent: it is indistinguishable from an empty room and from no
/// sensor at all, so reporting it would be inventing a fault out of silence.
enum class FaultCode : uint8_t {
  None = 0,
  /// Held HIGH for longer than any real room stays occupied. The window is
  /// minutes rather than seconds on purpose - somebody working at a bench pins a
  /// PIR HIGH legitimately for a long time, which was observed on device 17.
  StuckHigh,
  /// The pin could not be configured at all. Reported rather than retried,
  /// because a device that cannot read its sensor should still serve cards.
  InitFailed,
};

/// The policy as the server sent it. Absent policy is represented by
/// `present == false`, matching how Cards::Policy already works in this firmware -
/// an omitted object means "carry on", never "show nothing".
struct Policy {
  bool present = false;
  bool enabled = false;
  /// "legacyAlwaysOn" or "alwaysDim". Held as the server's own spelling so an
  /// added mode does not need a firmware change to be ignored safely.
  String fallbackMode;
  uint8_t activePercent = 100;
  uint8_t dimPercent = 25;
  uint32_t activeTimeoutSeconds = 120;
  uint32_t dimTimeoutSeconds = 180;
};

/// Configures the input pin. Safe to call whether or not a sensor is fitted:
/// GPIO35 is input-only at the silicon level, so nothing here can drive it.
void begin();

/// Applies a policy from a check-in response. A policy with `present == false`
/// puts the display into Fallback and stops motion affecting anything - it does
/// NOT blank the screen.
void applyPolicy(const Policy& policy);

/// Call every loop iteration. Samples the sensor, advances the timeouts and sets
/// brightness. Non-blocking: no delay(), no busy-wait, and it never holds the
/// loop across a fade (BL 03).
void service();

/// Records a touch as activity, whatever the sensor's state (BL 04, BL 08).
///
/// Returns true when the touch was CONSUMED as a wake and must not be delivered
/// to whatever is under the finger. `BL 07`: the first touch on a dark panel
/// wakes it and does nothing else, because a household cannot aim at a control
/// they cannot see.
bool noteTouchAndShouldSwallow();

/// True once a confirmed motion event has ever been seen this boot - the only
/// honest basis for reporting Present. Never true on a device with no sensor,
/// because nothing drives the pin.
bool hasEverDetectedMotion();

/// Capability to report to the server, or an empty string for "say nothing".
///
/// Only ever "Present" or "Faulted". Absent is not returnable by design - see the
/// file comment. An empty string leaves the server's own record and the operator
/// declaration untouched, which is what `CAP 05` requires of silence.
String capabilityToReport();

BacklightState state();
FaultCode fault();

/// Confirmed activity sessions since the last successful telemetry report, and
/// seconds since the last one. The age is deliberately not a timestamp: an age
/// decays into meaninglessness, whereas a time would still say somebody moved at
/// 02:14 a year later. There is nowhere on the server contract to put one.
uint32_t eventsSinceReport();
int32_t lastDetectedAgeSeconds();

/// Called after telemetry the server accepted, so counts are not double-reported.
/// Mirrors how the asset-decode and card-policy counters already clear.
void clearReportedCounters();

/// For telemetry: "enabled", "disabled" or "fallback". What the device is DOING,
/// as against what the policy permits.
String operatingState();

}  // namespace Motion
