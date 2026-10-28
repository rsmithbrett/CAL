#include "PowerProbe.h"

#include "AdcPins.h"
#include "Log.h"

namespace PowerProbe {
namespace {

// 34 and 35 on the fleet board, where they are ADC1_CH6 - documented elsewhere
// as the onboard LDR - and ADC1_CH7, input-only and broken out on P3.
//
// AdcPins::kNoPin on a target whose ADC1 does not reach them. The S3's ADC1 is
// GPIO1 to GPIO10 and its ADC2 is GPIO11 to GPIO20, per soc/adc_channel.h, so
// neither pad has a channel there. AdcPins.h records what
// analogReadMilliVolts() does with a pin outside that table, which is not to
// refuse it.
constexpr int8_t kPinA = AdcPins::adc1PinOrNone(34);
constexpr int8_t kPinB = AdcPins::adc1PinOrNone(35);

// The two pins above are derived from the SDK's channel table. These cross-check
// that result against the board gate, so a target where one side moves without
// the other fails the build instead of reporting zeros.
#if defined(CONFIG_IDF_TARGET_ESP32S3)
static_assert(kPinA == AdcPins::kNoPin && kPinB == AdcPins::kNoPin,
              "this target's ADC1 does not reach GPIO34 or GPIO35 - soc/adc_channel.h gives ADC1 "
              "as GPIO1-10 and ADC2 as GPIO11-20 - so this module holds no pin for either");
#else
static_assert(kPinA == 34 && kPinB == 35,
              "the probe pins are not GPIO34 and GPIO35 on this target, which is the pair "
              "PowerProbe.h's reasoning and the adc34Millivolts and adc35Millivolts telemetry "
              "fields are written against");
#endif

/// Averaged rather than sampled once. The ESP32's SAR ADC is visibly noisy -
/// tens of millivolts of jitter between consecutive reads is normal - and the
/// question being asked here is whether a reading TRACKS something (a battery
/// discharging, ambient light changing) or is just floating. Noise of the same
/// order as the signal would make a floating pin look like it was moving.
///
/// 16 samples is enough to settle that jitter and costs well under a
/// millisecond; this runs once per telemetry report, not per loop iteration.
constexpr uint8_t kSamples = 16;

uint32_t averageMillivolts(uint8_t pin) {
  uint32_t total = 0;
  for (uint8_t i = 0; i < kSamples; ++i) {
    total += analogReadMilliVolts(pin);
  }
  return total / kSamples;
}

/// One probe's reading, or 0 for a board that has no pin for it.
///
/// The pin test sits here rather than inside averageMillivolts() so that
/// function keeps the parameter and the body it has on the fleet board, where
/// the pins are constants and this test folds away at the call site.
///
/// 0 is what PowerProbe.h already defines as "the pin reads nothing", and it is
/// outside the range a connected pin can produce: at 11dB the ADC bottoms out
/// near 140 mV, which is what the fleet's four unconnected pins report. So a 0
/// in a log line or on telemetry is not a pin voltage of zero.
uint32_t probe(int8_t pin) {
  return pin < 0 ? 0 : averageMillivolts(static_cast<uint8_t>(pin));
}

}  // namespace

void begin() {
  // Set explicitly rather than relying on the core's default, which has changed
  // between core versions. The reading is meaningless without knowing the
  // attenuation it was taken at, so this pins it - see PowerProbe.h for why
  // 11dB specifically.
  analogSetAttenuation(ADC_11db);

  // A board with no probe pins reports that instead of two zeros. Both
  // millivoltsGpio34() and millivoltsGpio35() return 0 from here on, and
  // telemetry carries those zeros in adc34Millivolts and adc35Millivolts - see
  // PowerProbe.h for what a reader can take from them.
  if (kPinA < 0 && kPinB < 0) {
    Log::line("[power] no ADC pin on this board for either probe - this target's ADC1 reaches "
              "GPIO1-10 and its ADC2 GPIO11-20, so GPIO34 and GPIO35 have no channel. Both "
              "figures report 0 mV, which is below anything a connected pin can read");
    return;
  }

  // Logged at boot because these two figures are only interpretable against
  // each other and against what the device is plugged into at the time. A first
  // reading taken before anything else has run is the cleanest baseline there
  // is, and it costs one line.
  //
  // Note this lands in setup() and therefore before check-in authorises the
  // debug stream, so it will NOT reach the server - the same trap that hid the
  // [boot] restart reason and the [sd] mount cost. That is precisely why the
  // figures also ride on telemetry, and this line is only a convenience for
  // anyone watching over serial with a multimeter in hand.
  Log::printf("[power] probe at boot: GPIO34=%lumV GPIO35=%lumV (11dB attenuation, %u-sample "
              "average, no divider ratio applied - these are pin voltages, not battery voltage)",
              static_cast<unsigned long>(probe(kPinA)),
              static_cast<unsigned long>(probe(kPinB)),
              static_cast<unsigned>(kSamples));
}

uint32_t millivoltsGpio34() { return probe(kPinA); }

uint32_t millivoltsGpio35() { return probe(kPinB); }

}  // namespace PowerProbe
