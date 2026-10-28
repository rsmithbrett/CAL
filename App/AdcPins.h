#pragma once

#include <Arduino.h>

// sdkconfig.h before anything that reads a CONFIG_ name, for the reason
// Display.cpp gives at its own top: without it CONFIG_IDF_TARGET_ESP32S3 is
// undefined where a gate is evaluated and an S3 build takes the fleet board's
// branch while compiling clean.
#include "sdkconfig.h"

// The GPIO behind each ADC channel on this target. One macro per channel,
// named for both directions - ADC1_GPIO34_CHANNEL and ADC1_CHANNEL_6_GPIO_NUM
// are the same fact - and the set of them differs per chip. This is where the
// table below gets its numbers instead of listing pins in this repository.
#include <soc/adc_channel.h>

/// Which GPIOs `analogReadMilliVolts()` can read on the target this binary is
/// compiled for.
///
/// Two modules sample the ADC - PowerProbe and Motion - and each holds its own
/// pin numbers. This header holds the one thing they must agree on, which is
/// what counts as a readable pin at all.
///
/// **A pin outside the table is not refused by the core.** In core 3.3.11
/// `__analogReadMilliVolts()` (cores/esp32/esp32-hal-adc.c) calls
/// `adc_oneshot_io_to_channel()` without assigning its return value to the
/// `err` it then tests, so the `log_e("Pin %u is not ADC pin!")` branch beside
/// it cannot be reached. `adc_unit` and `channel` are left as whatever the
/// stack held, and `adc_unit` then indexes the file-scope `adc_handle` array,
/// which has `SOC_ADC_PERIPH_NUM` (2) entries. Disassembling the S3 image
/// confirms it: the call to `adc_oneshot_io_to_channel` is followed by no test
/// of its result, and the load four instructions later reads the unit from the
/// uninitialized frame slot and scales it by the 28-byte array stride.
/// `__analogRead()` in the same file does assign `err` and does refuse the pin,
/// so the two entry points behave differently on the same argument.
///
/// What follows from that is why the gates in PowerProbe.cpp and Motion.cpp
/// exist: a pin the table does not list returns no reliable value and reads
/// past the end of an array on the way, so the reading has to be skipped rather
/// than taken and interpreted.
namespace AdcPins {

/// True when `pin` is one of the GPIOs this target's ADC1 reaches.
///
/// Channels 0 through 7 exist on every target this firmware builds for; 8 and 9
/// are the S3's two extra ADC1 channels and are tested only where the SDK
/// defines them. ADC2 is deliberately absent: it cannot be sampled while WiFi
/// is up, and this device is never off WiFi, so an ADC2 pin is not a readable
/// pin here however the table lists it.
constexpr bool isAdc1Pin(int pin) {
  return pin == ADC1_CHANNEL_0_GPIO_NUM || pin == ADC1_CHANNEL_1_GPIO_NUM ||
         pin == ADC1_CHANNEL_2_GPIO_NUM || pin == ADC1_CHANNEL_3_GPIO_NUM ||
         pin == ADC1_CHANNEL_4_GPIO_NUM || pin == ADC1_CHANNEL_5_GPIO_NUM ||
         pin == ADC1_CHANNEL_6_GPIO_NUM || pin == ADC1_CHANNEL_7_GPIO_NUM
#if defined(ADC1_CHANNEL_8_GPIO_NUM)
         || pin == ADC1_CHANNEL_8_GPIO_NUM
#endif
#if defined(ADC1_CHANNEL_9_GPIO_NUM)
         || pin == ADC1_CHANNEL_9_GPIO_NUM
#endif
      ;
}

/// The value a module holds for "this board has no pin for that", matching the
/// convention Sound.h and CardManager.cpp already use for the speaker and the
/// action button.
constexpr int8_t kNoPin = -1;

/// `pin` if this target's ADC1 reaches it, `kNoPin` if it does not.
///
/// The pin a module holds is then derived from the SDK's table rather than
/// gated on a chip name, so the pin and the readability of the pin cannot come
/// apart. The static_asserts at each call site cross-check the result against
/// the board gate, which is what fails the build if either side moves.
constexpr int8_t adc1PinOrNone(int pin) {
  return isAdc1Pin(pin) ? static_cast<int8_t>(pin) : kNoPin;
}

}  // namespace AdcPins
