#pragma once

#include <stdint.h>

namespace PresenceTest {
constexpr uint8_t kHistorySize = 48;
constexpr uint32_t kHistoryIntervalMs = 250;

// Raw signal diagnostics, not the App's debounced occupancy/session logic.
// Empty history columns mean not sampled; low must not imply sensor absent.
class State {
 public:
  void reset() { *this = State{}; }
  void sample(uint32_t now, uint32_t millivolts, uint32_t threshold) {
    const bool next = millivolts >= threshold;
    if (hasReading && next != high) {
      if (changes != UINT32_MAX) ++changes;
      lastChangeMs = now;
    }
    high = next;
    mv = millivolts;
    if (!hasReading) {
      history_[head_] = high ? 2 : 1;
      head_ = (head_ + 1) % kHistorySize;
      historyMs_ = now;
    } else {
      const uint32_t steps = static_cast<uint32_t>(now - historyMs_) / kHistoryIntervalMs;
      const uint32_t count = steps < kHistorySize ? steps : kHistorySize;
      for (uint32_t i = 0; i < count; ++i) {
        // Missed samples remain unknown, never invented as a continuous signal.
        history_[head_] = i + 1 == count ? (high ? 2 : 1) : 0;
        head_ = (head_ + 1) % kHistorySize;
      }
      historyMs_ += steps * kHistoryIntervalMs;
    }
    hasReading = true;
  }
  uint8_t historyAt(uint8_t column) const {
    return history_[(head_ + column) % kHistorySize];
  }
  bool hasReading = false;
  bool high = false;
  uint32_t mv = 0;
  uint32_t changes = 0;
  uint32_t lastChangeMs = 0;

 private:
  uint32_t historyMs_ = 0;
  uint8_t history_[kHistorySize] = {};
  uint8_t head_ = 0;
};
}  // namespace PresenceTest
