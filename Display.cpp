#define LGFX_AUTODETECT
#include <LovyanGFX.hpp>
#include <LGFX_AUTODETECT.hpp>

#include <FS.h>
#include <LittleFS.h>

// Vendored copy of ricmoo/QRCode, renamed. A plain <qrcode.h> resolves to the
// ESP32 core's own esp_qrcode header instead of the library, which fails at
// compile time with the core's differently-named API suggested in its place.
// Renaming both the file and its include guard is what makes the local copy
// win; the functions inside are untouched and do not clash with esp_qrcode_*.
#include "CalQr.h"

#include "Display.h"

namespace Display {
namespace {

// LGFX_AUTODETECT senses the board rather than requiring a pin map. The panel
// this runs on is an LCDWIKI E32R28T whose HSPI-style pinout differs from the
// Sunton boards most published pin maps describe, and autodetect gets it right
// where a copied pin map does not.
LGFX lcd;

constexpr int kScreenW = 320;
constexpr int kScreenH = 240;

// Cached brand splash, written by the updater after the device authenticates.
// Raw RGB565 at a fixed size: the server prepares assets for the hardware, so
// the device needs no image decoder and no scaler.
constexpr const char* kBrandSplashPath = "/brand.565";
constexpr int kBrandW = 240;
constexpr int kBrandH = 120;

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

}  // namespace

void begin() {
  lcd.init();
  lcd.setRotation(1);  // 320x240 landscape
  lcd.setBrightness(255);
  clear();

  // Non-fatal: without it the brand splash simply never appears and CAL falls
  // back to the neutral one. Formatting on failure so a fresh unit ends up with
  // a usable filesystem rather than a permanently broken one.
  LittleFS.begin(true);
}

void showNeutralSplash() {
  clear();
  centeredText("Discover", 70, kInk, 4);
  centeredText("Around Me", 110, kAccent, 4);
  centeredText("starting", 170, kMuted, 2);
}

bool showBrandSplash() {
  if (!LittleFS.exists(kBrandSplashPath)) {
    return false;
  }

  File f = LittleFS.open(kBrandSplashPath, "r");
  if (!f) {
    return false;
  }

  const size_t expected = static_cast<size_t>(kBrandW) * kBrandH * 2;
  if (f.size() != expected) {
    // A truncated or wrong-sized asset is discarded rather than rendered as
    // garbage across the screen.
    f.close();
    return false;
  }

  clear();

  // Streamed a row at a time. A full-screen buffer would be 150KB and this
  // device has no PSRAM; one row is 480 bytes.
  static uint16_t row[kBrandW];
  const int x0 = (kScreenW - kBrandW) / 2;
  const int y0 = 40;
  for (int y = 0; y < kBrandH; ++y) {
    if (f.read(reinterpret_cast<uint8_t*>(row), sizeof(row)) != sizeof(row)) {
      f.close();
      return false;
    }
    lcd.pushImage(x0, y0 + y, kBrandW, 1, row);
  }
  f.close();
  return true;
}

void showStatus(const String& headline, const String& detail) {
  clear();
  centeredText(headline, 95, kInk, 2);
  if (detail.length() > 0) {
    centeredText(detail, 130, kMuted, 1);
  }
}

void showFailure(const String& headline, const String& whatToDo) {
  clear();
  centeredText(headline, 85, kWarn, 2);
  centeredText(whatToDo, 125, kMuted, 1);
}

void showQr(const String& url, const String& caption, const String& subCaption) {
  clear();

  // Version 6 at ECC LOW holds ~134 alphanumeric characters, comfortably more
  // than a setup URL, and stays coarse enough to scan off a 240-pixel panel.
  //
  // The buffer is sized here rather than by qrcode_getBufferSize(), which is a
  // runtime function in this library and so cannot size a static array. Same
  // arithmetic, evaluated at compile time.
  static constexpr uint8_t kQrVersion = 6;
  static constexpr size_t kQrModules = kQrVersion * 4 + 17;              // 41
  static constexpr size_t kQrBufferBytes = (kQrModules * kQrModules + 7) / 8;  // 211

  QRCode qr;
  static uint8_t data[kQrBufferBytes];
  if (qrcode_initText(&qr, data, kQrVersion, ECC_LOW, url.c_str()) != 0) {
    showFailure("Cannot display code", url);
    return;
  }

  const int quiet = 2;
  const int modules = qr.size + quiet * 2;
  const int scale = 140 / modules;
  const int side = modules * scale;
  const int x0 = (kScreenW - side) / 2;
  const int y0 = 12;

  lcd.fillRect(x0, y0, side, side, 0xFFFFFFu);
  for (uint8_t y = 0; y < qr.size; ++y) {
    for (uint8_t x = 0; x < qr.size; ++x) {
      if (qrcode_getModule(&qr, x, y)) {
        lcd.fillRect(x0 + (x + quiet) * scale, y0 + (y + quiet) * scale, scale,
                     scale, 0x000000u);
      }
    }
  }

  int y = y0 + side + 8;
  centeredText(caption, y, kInk, 2);
  y += 24;
  if (subCaption.length() > 0) {
    centeredText(subCaption, y, kAccent, 2);
    y += 22;
  }
  // The address in characters as well as in the code, because cameras fail.
  centeredText(url, y, kMuted, 1);
}

void showUpdateProgress(uint8_t percent, const String& version) {
  clear();
  centeredText("Updating", 70, kInk, 3);
  centeredText(version, 105, kMuted, 1);

  const int w = 240;
  const int h = 16;
  const int x0 = (kScreenW - w) / 2;
  const int y0 = 135;
  lcd.drawRect(x0, y0, w, h, kMuted);
  lcd.fillRect(x0 + 2, y0 + 2, ((w - 4) * percent) / 100, h - 4, kAccent);

  centeredText(String(percent) + "%", y0 + h + 12, kMuted, 1);
  centeredText("Do not unplug", y0 + h + 34, kWarn, 1);
}

}  // namespace Display
