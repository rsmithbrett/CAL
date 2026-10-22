#pragma once

#include <algorithm>
#include <cstdint>

namespace PiLayout {

struct Rect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

struct Frame {
  Rect safe;
  Rect header;
  Rect graphic;
  Rect footer;
};

inline Rect contain(Rect available, int sourceWidth, int sourceHeight) {
  if (available.w <= 0 || available.h <= 0 || sourceWidth <= 0 ||
      sourceHeight <= 0) return {};
  const int64_t horizontal = int64_t(available.w) * sourceHeight;
  const int64_t vertical = int64_t(available.h) * sourceWidth;
  const int width = horizontal <= vertical
                        ? available.w
                        : int(int64_t(available.h) * sourceWidth / sourceHeight);
  const int height = horizontal <= vertical
                         ? int(int64_t(available.w) * sourceHeight / sourceWidth)
                         : available.h;
  return {available.x + (available.w - width) / 2,
          available.y + (available.h - height) / 2, width, height};
}

inline Frame makeFrame(int width, int height, int graphicWidth,
                       int graphicHeight) {
  if (width <= 0 || height <= 0) return {};
  const int margin = std::max(8, std::min(width, height) / 24);
  Rect safe{margin, margin, std::max(0, width - 2 * margin),
            std::max(0, height - 2 * margin)};
  const int headerHeight = std::min(safe.h / 4, std::max(34, height / 8));
  const int footerHeight = std::min(safe.h / 4, std::max(30, height / 10));
  Rect header{safe.x, safe.y, safe.w, headerHeight};
  Rect footer{safe.x, safe.y + safe.h - footerHeight, safe.w, footerHeight};
  Rect body{safe.x, header.y + header.h + margin / 2, safe.w,
            std::max(0, footer.y - header.y - header.h - margin)};
  return {safe, header, contain(body, graphicWidth, graphicHeight), footer};
}

}  // namespace PiLayout
