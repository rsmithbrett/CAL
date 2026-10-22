#pragma once

#include <cstdint>

// A small, platform-neutral part of the card rotation. CardManager uses this
// with its real registry; the Pi client can use the same transitions without
// pulling Arduino, display, or network code into its process.
namespace ScheduleCore {

template <typename Card>
void tickActive(Card* cards, uint8_t count) {
  for (uint8_t i = 0; i < count; ++i) {
    if (cards[i].active && cards[i].cardsSince < UINT16_MAX) {
      ++cards[i].cardsSince;
    }
  }
}

// eligible checks both the card kind and whether it currently has content.
// earlier implements the caller's policy ordering, including its tie break.
// The caller resets the selected card's counter only after it is shown.
template <typename Card, typename Eligible, typename Earlier>
int8_t dueInterstitial(const Card* cards, uint8_t count,
                       Eligible eligible, Earlier earlier) {
  int8_t best = -1;
  for (uint8_t i = 0; i < count; ++i) {
    if (cards[i].interleaveEvery == 0 || !eligible(i) ||
        cards[i].cardsSince <= cards[i].interleaveEvery) {
      continue;
    }
    if (best < 0 || earlier(i, static_cast<uint8_t>(best))) {
      best = static_cast<int8_t>(i);
    }
  }
  return best;
}

// Ordered list traversal shared by ESP32 and Pi. Registration index tie breaks
// stay with the caller; an absent/ineligible cursor starts at the first card.
template <typename Eligible, typename Earlier>
int8_t nextShowable(uint8_t count, int8_t after, Eligible eligible, Earlier earlier) {
  int8_t first = -1, next = -1;
  for (uint8_t i = 0; i < count; ++i) {
    if (!eligible(i)) continue;
    if (first < 0 || earlier(i, static_cast<uint8_t>(first))) first = i;
    if (after >= 0 && earlier(static_cast<uint8_t>(after), i) &&
        (next < 0 || earlier(i, static_cast<uint8_t>(next)))) next = i;
  }
  return next >= 0 ? next : first;
}

}  // namespace ScheduleCore
