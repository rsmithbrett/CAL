#include "Layout.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <utility>

int main() {
  for (auto [width, height] : {std::pair{320, 240}, std::pair{1280, 720},
                               std::pair{1920, 1080}, std::pair{800, 1280}}) {
    auto f = PiLayout::makeFrame(width, height, 4, 3);
    assert(f.safe.x > 0 && f.safe.y > 0);
    assert(f.graphic.w > 0 && f.graphic.h > 0);
    assert(f.graphic.x >= f.safe.x && f.graphic.y >= f.header.y + f.header.h);
    assert(f.graphic.x + f.graphic.w <= f.safe.x + f.safe.w);
    assert(f.graphic.y + f.graphic.h < f.footer.y);
    assert(int64_t(f.graphic.w) * 3 <= int64_t(f.graphic.h) * 4 + 3);
    assert(int64_t(f.graphic.h) * 4 <= int64_t(f.graphic.w) * 3 + 4);
  }
  assert(PiLayout::contain({0, 0, 100, 100}, 0, 5).w == 0);
  assert(PiLayout::makeFrame(0, 240, 4, 3).graphic.w == 0);
  std::cout << "layout: 30 assertions passed across four viewports\n";
}
