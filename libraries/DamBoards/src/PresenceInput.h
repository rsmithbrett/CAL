#pragma once

#include <stdint.h>
#include "sdkconfig.h"
#include <soc/adc_channel.h>

// Shared by CAL diagnostics and the App. Do not substitute GPIO35 on the S3:
// it is not an ADC1 input there, and no replacement sensor wiring is mapped.
namespace PresenceInput {
#if defined(ADC1_GPIO35_CHANNEL)
constexpr int8_t kPin = 35;
#else
constexpr int8_t kPin = -1;
#endif
#if defined(CONFIG_IDF_TARGET_ESP32S3)
static_assert(kPin == -1, "Panellan has no mapped presence input");
#else
static_assert(kPin == 35, "Original ESP32 presence input must be ADC1 GPIO35");
#endif
constexpr bool kSupported = kPin >= 0;
constexpr uint32_t kHighThresholdMillivolts = 1500;
constexpr uint32_t kSampleIntervalMs = 50;
constexpr uint32_t kStabilizeMs = 30000;
}  // namespace PresenceInput
