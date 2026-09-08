// SD.h MUST come before LovyanGFX.hpp, not after - same trap App/Display.cpp
// and CAL's own root Display.cpp both document: LovyanGFX auto-detects
// SD-card image support by checking whether the SD library's own include
// guard is already defined, and including it afterwards fails to compile
// with "abstract type DataWrapperT<fs::SDFS>".
#include <SD.h>
#include <cstdlib>

#define LGFX_AUTODETECT
#include <LovyanGFX.hpp>
#include <LGFX_AUTODETECT.hpp>

#include "Display.h"
#include "Log.h"

namespace Display {
namespace {

// Same panel, same autodetect approach as CAL and App - see README's
// "Hardware" section for why LGFX_AUTODETECT over a hand pin map on this
// specific board.
LGFX lcd;

constexpr int kScreenW = 320;
constexpr int kScreenH = 240;

// Plain black/white, no day/night theme - this sketch draws no product
// content and runs in a diagnostic context (someone standing over the
// device with it in hand), so there is no reason to match ambient light.
constexpr uint32_t kBg = 0x000000u;
constexpr uint32_t kInk = 0xFFFFFFu;
constexpr uint32_t kMuted = 0x9A9A9Au;
constexpr uint32_t kPass = 0x30D030u;
constexpr uint32_t kFail = 0xE03030u;
constexpr uint32_t kWarn = 0xEDA100u;

void clear() { lcd.fillScreen(kBg); }

// Wraps `text` across up to a few lines centered horizontally, starting at
// `topY`, returning the number of lines drawn - same technique App's own
// Display.cpp uses for showStatus()/showFailure(), trimmed to just what
// this sketch needs (no muted/day-night color selection).
int wrappedCenteredText(const String& text, int topY, uint32_t colour, uint8_t textSize,
                        int lineHeight, int maxLines) {
  lcd.setFont(&fonts::Font0);
  lcd.setTextSize(textSize);
  lcd.setTextColor(colour, kBg);
  lcd.setTextDatum(top_center);

  const int maxWidth = kScreenW - 20;
  int lineStart = 0;
  int cursorY = topY;
  int linesDrawn = 0;

  while (lineStart < static_cast<int>(text.length()) && linesDrawn < maxLines) {
    int breakAt = text.length();
    for (int probe = lineStart + 1; probe <= static_cast<int>(text.length()); ++probe) {
      const String candidate = text.substring(lineStart, probe);
      if (lcd.textWidth(candidate) <= maxWidth) {
        breakAt = probe;
      } else {
        break;
      }
    }
    if (breakAt == lineStart) {
      breakAt = lineStart + 1;  // a single character wider than the line - draw it anyway
    }
    lcd.drawString(text.substring(lineStart, breakAt), kScreenW / 2, cursorY);
    lineStart = breakAt;
    cursorY += lineHeight;
    ++linesDrawn;
  }
  return linesDrawn;
}

// --- The shared, only-grows read buffer for PNG decode - see Display.h's
// own remarks on drawPngFromSdTest() for why this exists at all: it is the
// exact fix App/Display.cpp applies to the read-buffer heap-fragmentation
// bug this whole sketch exists to catch earlier next time.
uint8_t* gFileBuffer = nullptr;
size_t gFileBufferCapacity = 0;

bool ensureFileBufferCapacity(size_t needed) {
  if (needed <= gFileBufferCapacity) {
    return true;
  }
  uint8_t* grown = static_cast<uint8_t*>(realloc(gFileBuffer, needed));
  if (grown == nullptr) {
    return false;
  }
  gFileBuffer = grown;
  gFileBufferCapacity = needed;
  return true;
}

}  // namespace

void begin() {
  lcd.init();
  lcd.setRotation(1);  // 320x240 landscape, same as CAL/App
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

bool fillColorTest(uint32_t color888) {
  // fillScreen() has no failure return in LovyanGFX's API - see Display.h's
  // own remarks. This reports true unconditionally; a genuine driver fault
  // here would hang or crash rather than return false, which the caller's
  // own overall timeout/watchdog (not this function) is what would catch.
  lcd.fillScreen(color888);
  return true;
}

bool textRenderTest(uint8_t sizeMultiplier) {
  clear();
  lcd.setFont(&fonts::FreeSansBold9pt7b);
  lcd.setTextSize(sizeMultiplier);
  lcd.setTextColor(kInk, kBg);
  lcd.setTextDatum(middle_center);
  const String sample = "SelfTest 0123 ABC";
  const int32_t drawnWidth = lcd.drawString(sample, kScreenW / 2, kScreenH / 2);
  return drawnWidth > 0;
}

bool drawPngFromSdTest(const String& path) {
  clear();
  File file = SD.open(path, FILE_READ);
  if (!file) {
    Log::printf("[display] could not open %s to read into memory", path.c_str());
    return false;
  }
  const size_t fileSize = file.size();
  if (fileSize == 0) {
    file.close();
    Log::printf("[display] %s is empty on SD - nothing to decode", path.c_str());
    return false;
  }
  if (!ensureFileBufferCapacity(fileSize)) {
    file.close();
    Log::printf(
        "[display] out of memory growing the read buffer to %u bytes for %s "
        "(freeHeap=%u maxAllocHeap=%u)",
        static_cast<unsigned>(fileSize), path.c_str(), static_cast<unsigned>(ESP.getFreeHeap()),
        static_cast<unsigned>(ESP.getMaxAllocHeap()));
    return false;
  }
  const size_t bytesRead = file.read(gFileBuffer, fileSize);
  file.close();
  if (bytesRead != fileSize) {
    Log::printf("[display] short read on %s (%u of %u bytes)", path.c_str(),
                static_cast<unsigned>(bytesRead), static_cast<unsigned>(fileSize));
    return false;
  }

  const bool ok =
      lcd.drawPng(gFileBuffer, fileSize, 0, 0, 0, 0, 0, 0, 0.0f, 0.0f, middle_center);
  if (!ok) {
    Log::printf("[display] failed to decode/draw %s (freeHeap=%u maxAllocHeap=%u)", path.c_str(),
                static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  }
  return ok;
}

void showResults(const ResultLine* lines, uint8_t count, uint16_t totalPassed,
                  uint16_t totalRun) {
  clear();
  lcd.setFont(&fonts::Font0);
  lcd.setTextDatum(top_left);
  lcd.setTextSize(2);
  lcd.setTextColor(kInk, kBg);
  lcd.drawString("SelfTest Results", 8, 6);

  int y = 34;
  for (uint8_t i = 0; i < count; ++i) {
    lcd.setTextSize(1);
    const uint32_t statusColour = lines[i].passed ? kPass : kFail;
    lcd.setTextColor(statusColour, kBg);
    lcd.setTextDatum(top_left);
    const String status = lines[i].passed ? "[PASS] " : "[FAIL] ";
    lcd.drawString(status + lines[i].label, 8, y);
    y += 16;
    if (lines[i].detail.length() > 0) {
      lcd.setTextColor(kMuted, kBg);
      String detail = lines[i].detail;
      // Truncate rather than wrap - this screen is a dashboard, not prose;
      // the full detail always also reaches the remote debug stream and the
      // JSON report (see Report.cpp), so nothing is lost, only not repeated
      // here in full.
      const int maxWidth = kScreenW - 16;
      while (detail.length() > 1 && lcd.textWidth(detail) > maxWidth) {
        detail.remove(detail.length() - 1);
      }
      lcd.drawString(detail, 16, y);
      y += 14;
    }
    y += 4;
  }

  lcd.setTextSize(1);
  lcd.setTextDatum(bottom_left);
  const uint32_t summaryColour = (totalPassed == totalRun) ? kPass : kWarn;
  lcd.setTextColor(summaryColour, kBg);
  lcd.drawString(String(totalPassed) + " / " + String(totalRun) + " passed", 8, kScreenH - 6);
}

}  // namespace Display
