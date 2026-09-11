#pragma once

#include <Arduino.h>

/// Reads the two ADC pins that could plausibly carry a battery voltage on this
/// board, and reports both raw, so the question "is there a battery sense line,
/// and which pin is it on" can be settled by measurement instead of by reading
/// conflicting pinout pages.
///
/// **Why this exists.** CheckIn.cpp has sent `batteryPercent = 100` and
/// `charging = true` as literal constants since it was written, documented in
/// CheckIn.h as "This board has no battery (ELEGOO/CYD is USB-powered)". That
/// is a placeholder wearing the shape of a measurement: every device on the
/// fleet reports identical values, and /diag/telemetry renders a Battery column
/// that can never say anything true. This fleet has already been misled twice
/// by exactly that shape - `totalBoots` counting restarts with no cause, and
/// `ESP.getMaxAllocHeap()` returning a constant 32,756 that reads as a real
/// figure - so a third one is worth closing.
///
/// The premise turns out to be wrong anyway: these units DO charge an onboard
/// battery. What is not established is whether the charge circuit exposes
/// anything to the ESP32.
///
/// **Why two pins and no decision between them.** Published pinouts for the
/// ESP32-2432S028 disagree, and the board has revisions that genuinely differ:
///
///   - GPIO34 is ADC1_CH6 and is widely documented as the onboard LDR light
///     sensor on this board. If that is true here, its reading will track
///     ambient light - cover the panel and it moves. That makes the LDR
///     self-identifying rather than something to be taken on trust.
///   - GPIO35 is ADC1_CH7, input-only, and broken out on the P3 header. It is
///     the pin a divider from VBAT would most plausibly use, and input-only is
///     exactly what a divider wants.
///
/// Picking one and hard-coding a divider ratio would produce a number that
/// looks authoritative and might be the light sensor. Reading both and sending
/// millivolts settles it in one telemetry cycle across every device at once.
///
/// **GPIO27 is deliberately not probed.** It is free and ADC-capable, but it is
/// on ADC2, and ADC2 cannot be read while WiFi is active on the ESP32. This
/// device is never off WiFi, so that pin could not carry a usable battery sense
/// line whatever it is wired to.
///
/// **No divider ratio is applied.** The ratio is unknown and guessing it would
/// bake a wrong constant into the first data ever gathered. These are the pin
/// voltages as measured; converting to battery volts is a later change, made
/// once a multimeter has confirmed both the divider's existence and its values.
///
/// Safe to call at any time: both pins are read-only, neither is driven, and
/// GPIO35 is input-only at the silicon level so it cannot be driven by mistake.
namespace PowerProbe {

/// Configures ADC attenuation once. Call from setup() before read().
///
/// 11dB attenuation (ESP-IDF's ADC_ATTEN_DB_12) gives a usable range to roughly
/// 2,500mV with reasonable linearity, and saturates near 3,100mV. A single LiPo
/// cell at 4.2V through a conventional 2:1 divider lands near 2,100mV, which
/// sits inside that range with headroom - so if a divider exists on either pin,
/// this attenuation will read it rather than clip it.
void begin();

/// Pin voltage in millivolts, averaged over several samples.
///
/// `analogReadMilliVolts()` rather than `analogRead()` on purpose: it applies
/// the per-chip ADC calibration burned into eFuse at manufacture, which matters
/// because the ESP32's raw ADC is noticeably non-linear and varies unit to
/// unit. A raw count would have to be calibrated by hand per device; this does
/// not. Returns 0 if the pin reads nothing.
uint32_t millivoltsGpio34();
uint32_t millivoltsGpio35();

}  // namespace PowerProbe
