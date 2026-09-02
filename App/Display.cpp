#define LGFX_AUTODETECT
#include <LovyanGFX.hpp>
#include <LGFX_AUTODETECT.hpp>

#include "Display.h"

namespace Display {
namespace {

// Same panel as CAL - see CAL's own Display.cpp for why autodetect over a pin
// map. No CalQr.h here: the App never renders a QR code.
LGFX lcd;

constexpr int kScreenW = 320;
constexpr int kScreenH = 240;

constexpr uint32_t kBg = 0x000000u;
constexpr uint32_t kInk = 0xFFFFFFu;
constexpr uint32_t kMuted = 0x9A9A9Au;
constexpr uint32_t kAccent = 0x2A78D6u;
constexpr uint32_t kWarn = 0xEDA100u;

void clear() {
  lcd.fillScreen(kBg);
}

void centeredText(const String& text, int y, uint32_t colour, uint8_t size) {
  lcd.setTextColor(colour, kBg);
  lcd.setTextSize(size);
  lcd.setTextDatum(top_center);
  lcd.drawString(text, kScreenW / 2, y);
}

// Identical to CAL's Display.cpp helper of the same name - see there for why
// greedy word-wrap measured with real font metrics matters: server-supplied
// strings (a card's shortForecast, a content-gate refusal message) arrive with
// no length this file controls.
int wrappedCenteredText(const String& text, int y, uint32_t colour, uint8_t size,
                        int lineHeight, int maxLines) {
  lcd.setTextColor(colour, kBg);
  lcd.setTextSize(size);
  lcd.setTextDatum(top_center);

  constexpr int kMargin = 8;
  const int maxWidth = kScreenW - kMargin * 2;
  const int textLen = static_cast<int>(text.length());

  int lineStart = 0;
  int lastSpace = -1;
  int cursorY = y;
  int linesDrawn = 0;

  for (int i = 0; i <= textLen && linesDrawn < maxLines; ++i) {
    const bool atEnd = (i == textLen);
    const bool isSpace = !atEnd && text.charAt(i) == ' ';
    if (isSpace) {
      lastSpace = i;
    }
    if (!atEnd && !isSpace) {
      continue;
    }

    const String candidate = text.substring(lineStart, i);
    if (lcd.textWidth(candidate) <= maxWidth) {
      if (atEnd) {
        lcd.drawString(candidate, kScreenW / 2, cursorY);
        linesDrawn++;
      }
      continue;
    }

    const int breakAt = (lastSpace > lineStart) ? lastSpace : i;
    lcd.drawString(text.substring(lineStart, breakAt), kScreenW / 2, cursorY);
    cursorY += lineHeight;
    linesDrawn++;
    lineStart = (lastSpace > lineStart) ? lastSpace + 1 : breakAt;
    lastSpace = -1;
    i = lineStart - 1;
  }

  return linesDrawn;
}

}  // namespace

void begin() {
  lcd.init();
  lcd.setRotation(1);  // 320x240 landscape
  lcd.setBrightness(255);
  clear();
}

void showStatus(const String& headline, const String& detail) {
  clear();
  const int headlineLines = wrappedCenteredText(headline, 85, kInk, 2, 22, 3);
  if (detail.length() > 0) {
    wrappedCenteredText(detail, 85 + headlineLines * 22 + 12, kMuted, 1, 14, 3);
  }
}

void showFailure(const String& headline, const String& whatToDo) {
  clear();
  const int headlineLines = wrappedCenteredText(headline, 75, kWarn, 2, 22, 3);
  wrappedCenteredText(whatToDo, 75 + headlineLines * 22 + 12, kMuted, 1, 14, 3);
}

void showWeatherCard(const String& location, int temperature, const String& unit,
                     const String& shortForecast, const String& updatedAt) {
  clear();

  if (location.length() > 0) {
    centeredText(location, 20, kMuted, 2);
  }

  centeredText(String(temperature) + "\xC2\xB0" + unit, 55, kInk, 6);

  const int forecastLines = wrappedCenteredText(shortForecast, 145, kAccent, 2, 26, 2);

  if (updatedAt.length() > 0) {
    centeredText(updatedAt, 145 + forecastLines * 26 + 20, kMuted, 1);
  }
}

}  // namespace Display
