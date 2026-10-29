#include "ScheduleCore.h"

#include <cassert>
#include <cstdint>
#include <iostream>

struct Card {
  bool active;
  bool interstitial;
  bool hasContent;
  uint16_t interleaveEvery;
  uint16_t cardsSince;
  int order;
};

static int8_t due(const Card* cards, uint8_t count) {
  return ScheduleCore::dueInterstitial(
      cards, count,
      [cards](uint8_t i) {
        return cards[i].active && cards[i].interstitial && cards[i].hasContent;
      },
      [cards](uint8_t a, uint8_t b) {
        return cards[a].order < cards[b].order ||
               (cards[a].order == cards[b].order && a < b);
      });
}

int main() {
  Card cards[] = {
      {true, true, true, 2, 0, 10},
      {true, true, true, 3, 0, 5},
      {true, false, true, 0, 0, 0},
  };
  for (int step = 0; step < 2; ++step) {
    ScheduleCore::tickActive(cards, 3);
    assert(due(cards, 3) == -1);
  }
  ScheduleCore::tickActive(cards, 3);
  assert(due(cards, 3) == 0);  // Exceeds N, not equal to N.
  cards[0].cardsSince = 0;     // Showing one leaves the other's counter alone.
  ScheduleCore::tickActive(cards, 3);
  assert(due(cards, 3) == 1);
  cards[1].hasContent = false;
  assert(due(cards, 3) == -1);
  cards[1].hasContent = true;
  cards[0].cardsSince = 4;
  cards[1].cardsSince = 5;
  assert(due(cards, 3) == 1);  // Lower policy order wins.
  cards[1].order = 10;
  assert(due(cards, 3) == 0);  // Equal order keeps registration order.
  cards[0].active = false;
  assert(due(cards, 3) == 1);
  cards[1].cardsSince = UINT16_MAX;
  ScheduleCore::tickActive(cards, 3);
  assert(cards[1].cardsSince == UINT16_MAX);  // Never wraps to zero.
  std::cout << "schedule core: 9 assertions passed\n";
}
