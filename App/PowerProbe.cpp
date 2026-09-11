#include "PowerProbe.h"

#include "Log.h"

namespace PowerProbe {
namespace {

constexpr uint8_t kPinA = 34;  // ADC1_CH6 - documented elsewhere as the onboard LDR
constexpr uint8_t kPinB = 35;  // ADC1_CH7 - input-only, broken out on P3

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

}  // namespace

void begin() {
  // Set explicitly rather than relying on the core's default, which has changed
  // between core versions. The reading is meaningless without knowing the
  // attenuation it was taken at, so this pins it - see PowerProbe.h for why
  // 11dB specifically.
  analogSetAttenuation(ADC_11db);

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
              static_cast<unsigned long>(averageMillivolts(kPinA)),
              static_cast<unsigned long>(averageMillivolts(kPinB)),
              static_cast<unsigned>(kSamples));
}

uint32_t millivoltsGpio34() { return averageMillivolts(kPinA); }

uint32_t millivoltsGpio35() { return averageMillivolts(kPinB); }

}  // namespace PowerProbe
