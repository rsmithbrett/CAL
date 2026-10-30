#pragma once

#include <stdint.h>
#include "sdkconfig.h"
#include <PresenceInput.h>

// A board allowlist, not a scan that changes every pad to INPUT. The other
// GPIO numbers remain visible in the inventory but are never reconfigured.
namespace GpioMonitor {
enum class Kind : uint8_t { Analog, Digital, Reserved, Unavailable };
#if defined(CONFIG_IDF_TARGET_ESP32S3)
static_assert(ADC1_GPIO10_CHANNEL >= 0, "GPIO10 must be an ADC1 input on Panellan");
constexpr uint8_t kPortCount = 49;
// Vendor general-purpose header set recorded in App/CardManager.cpp. Other
// apparently spare silicon pads have not been verified as board breakouts.
constexpr uint8_t kInputs[] = {10, 11, 12, 13, 14, 21};
constexpr Kind kind(uint8_t pin) {
  if (pin == 10) return Kind::Analog;
  if ((pin >= 11 && pin <= 14) || pin == 21) return Kind::Digital;
  if (pin >= 49 || (pin >= 22 && pin <= 25)) return Kind::Unavailable;
  return Kind::Reserved;
}
#else
static_assert(ADC1_GPIO34_CHANNEL >= 0, "GPIO34 must be an ADC1 input on the original board");
constexpr uint8_t kPortCount = 40;
constexpr uint8_t kInputs[] = {27, 34, 35};
constexpr Kind kind(uint8_t pin) {
  if (pin == 34 || pin == 35) return Kind::Analog;
  if (pin == 27) return Kind::Digital;
  if (pin >= 40 || pin == 20 || pin == 24 || (pin >= 28 && pin <= 31)) return Kind::Unavailable;
  return Kind::Reserved;
}
#endif
constexpr uint8_t kInputCount = sizeof(kInputs) / sizeof(kInputs[0]);
constexpr uint8_t kEventCount = 8;
constexpr uint32_t kStableMs = 150;

struct Input {
  uint32_t value = 0;
  uint32_t changes = 0;
  uint32_t lastChangeMs = 0;
  uint32_t candidateMs = 0;
  bool sampled = false, high = false, candidate = false;
};
struct Event {
  uint32_t when = 0, value = 0;
  uint8_t pin = 0;
  bool high = false;
};
class State {
 public:
  void reset() { *this = State{}; }
  const Input* input(uint8_t pin) const {
    for (uint8_t i = 0; i < kInputCount; ++i) if (kInputs[i] == pin) return &inputs_[i];
    return nullptr;
  }
  void sample(uint8_t pin, uint32_t now, uint32_t value) {
    for (uint8_t i = 0; i < kInputCount; ++i) {
      if (kInputs[i] != pin) continue;
      Input& v = inputs_[i];
      const bool high = kind(pin) == Kind::Analog ? value >= PresenceInput::kHighThresholdMillivolts : value != 0;
      v.value = value;
      if (!v.sampled) {
        v.sampled = true; v.high = high; v.candidate = high; v.candidateMs = now;
        return; // The first reading establishes a baseline, not an event.
      }
      if (v.candidate != high) { v.candidate = high; v.candidateMs = now; }
      if (v.high != high && static_cast<uint32_t>(now - v.candidateMs) >= kStableMs) {
        v.high = high; v.lastChangeMs = now;
        if (v.changes != UINT32_MAX) ++v.changes;
        events_[head_] = Event{now, value, pin, high};
        head_ = (head_ + 1) % kEventCount;
        if (count_ < kEventCount) ++count_;
      }
      return;
    }
  }
  uint8_t count() const { return count_; }
  const Event& event(uint8_t newestIndex) const {
    return events_[(head_ + kEventCount - 1 - newestIndex) % kEventCount];
  }
 private:
  Input inputs_[kInputCount];
  Event events_[kEventCount];
  uint8_t head_ = 0, count_ = 0;
};
}  // namespace GpioMonitor
