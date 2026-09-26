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
// wrappedCenteredText() says when it has ellipsized. A boot screen whose
// message was cut is exactly the kind of thing nobody notices until a
// household reads half an instruction, and the journal survives the reboot
// that Serial does not.
#include "Journal.h"
#include <string.h>

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

// Whether the panel has actually been brought up this boot. See startPanel().
bool gPanelStarted = false;

/// Brings the panel up if it is not up yet, and does nothing if it is.
///
/// **Why this is not simply `begin()`'s body.** CAL now has one path - the
/// immediate handover on a therapeutic restart - where it deliberately draws
/// nothing at all and therefore never calls `begin()`. See
/// BOOT_SCREEN_OWNERSHIP.md section 6. On that path CAL can still need the
/// panel afterwards, for the one reason that matters: `bootApplication()` can
/// return instead of restarting, and the `haltWithFailure()` that follows is
/// exactly the screen a recoverable-but-not-starting device is rescued by. A
/// display that was skipped and cannot be brought back would turn the quiet
/// path into the dark-and-apparently-dead failure this whole design exists to
/// avoid.
///
/// So every public entry point below calls this first, and the property that
/// buys is worth stating: there is no reachable state in which CAL wants to
/// draw and cannot. It costs one bool and one branch per draw.
///
/// Deliberately NOT exposed in Display.h. Nothing outside this file needs to
/// ask - CAL.ino expresses "stay quiet" by not calling a draw function, not by
/// managing display state - and a public entry point with no caller is its own
/// class of bug here (see ci/build-firmware.sh's Motion gate).
void startPanel() {
  if (gPanelStarted) {
    return;
  }
  gPanelStarted = true;

  lcd.init();
  lcd.setRotation(1);  // 320x240 landscape
  lcd.setBrightness(255);
  clear();

  // Non-fatal: without it the brand splash simply never appears and CAL falls
  // back to the neutral one. Formatting on failure so a fresh unit ends up with
  // a usable filesystem rather than a permanently broken one.
  LittleFS.begin(true);
}

void centeredText(const String& text, int y, uint32_t colour, uint8_t size) {
  lcd.setTextColor(colour, kBg);
  lcd.setTextSize(size);
  lcd.setTextDatum(top_center);
  lcd.drawString(text, kScreenW / 2, y);
}

// Greedy word-wrap, measured with the real font metrics (lcd.textWidth) rather than a
// guessed characters-per-line budget - the two failure/status text callers below take
// server-supplied strings verbatim ("the wording shown on screen comes from the server
// rather than from a table compiled in here", per Enrollment.cpp), so CAL has no control
// over their length and cannot assume any sentence fits on one line at any text size.
// Returns how many lines it actually drew, so a caller stacking more text below can place
// it after however many lines this one sentence turned out to need, rather than at a fixed
// offset sized for a single line.
//
// **THIS BROKE AT THE WRONG SPACE, AND THE APP'S COPY OF IT WAS PHOTOGRAPHED DOING SO.**
// The old version recorded the space at i as a break point BEFORE measuring the candidate
// ending at i, so when the overflow was discovered at a space the break point was that same
// space: the line that had just been measured and rejected was the line that got drawn, and
// the panel clipped whatever hung off the right edge. App/Display.cpp carried the identical
// function and the identical ordering, and a notice card came back from real hardware
// reading "Test device: this rotation h" with the "as" of "has" nowhere on the screen. CAL
// has never been photographed failing this way, but it is the same code drawing the same
// kind of server-supplied string, so it is fixed in the same pass.
//
// App/ got a full layout routine out of this (see TEXT_LAYOUT_DESIGN.md). CAL gets the
// correction and an ellipsis and nothing more: this file has one call site shape, centred
// text on a boot screen, and importing a layout module into the factory partition would buy
// nothing it can use.
//
// Three changes, then, against the old body:
//
//   1. A line breaks at the last whitespace STRICTLY BEFORE the overflowing word. Nothing
//      is drawn that has not been measured and accepted at the width it is drawn at.
//   2. Running out of lines ellipsizes the last one with a visible "..." instead of
//      dropping the remainder with nothing to show for it. Three ASCII periods, not U+2026:
//      the bitmap face here has no Unicode coverage and a single-glyph ellipsis would draw
//      as a box, which is a worse lie than the truncation it is disclosing.
//   3. No Arduino String on the path. The old body allocated one per candidate measurement
//      and one per line drawn; this uses a fixed stack buffer, because CAL runs on the same
//      board whose largest contiguous block decides whether TLS can open.
constexpr size_t kWrapLineBytes = 128;

int wrappedCenteredText(const String& text, int y, uint32_t colour, uint8_t size,
                        int lineHeight, int maxLines) {
  lcd.setTextColor(colour, kBg);
  lcd.setTextSize(size);
  lcd.setTextDatum(top_center);

  constexpr int kMargin = 8;
  const int maxWidth = kScreenW - kMargin * 2;

  const char* source = text.c_str();
  const size_t length = text.length();
  char line[kWrapLineBytes];
  const size_t capacity = sizeof(line) - 1;

  size_t cursor = 0;
  int cursorY = y;
  int linesDrawn = 0;

  // Any byte at or below 0x20 counts as a break, not just ' '. A '\n' in a server message
  // would otherwise be handed to drawString() and rendered as whatever box glyph this face
  // has for it, in the middle of a sentence a household is trying to act on.
  auto isBreak = [](char c) { return static_cast<unsigned char>(c) <= 0x20; };

  while (cursor < length && linesDrawn < maxLines) {
    while (cursor < length && isBreak(source[cursor])) {
      ++cursor;
    }
    if (cursor >= length) {
      break;
    }

    size_t lineEnd = cursor;
    size_t scan = cursor;
    while (scan < length) {
      size_t wordEnd = scan;
      while (wordEnd < length && !isBreak(source[wordEnd])) {
        ++wordEnd;
      }
      if (wordEnd - cursor > capacity) {
        break;
      }
      for (size_t i = cursor; i < wordEnd; ++i) {
        line[i - cursor] = isBreak(source[i]) ? ' ' : source[i];
      }
      line[wordEnd - cursor] = '\0';
      if (lcd.textWidth(line) > maxWidth) {
        break;
      }
      lineEnd = wordEnd;
      scan = wordEnd;
      while (scan < length && isBreak(source[scan])) {
        ++scan;
      }
    }

    if (lineEnd == cursor) {
      // One word wider than the whole screen. Break it mid-word and carry the rest onto the
      // next line rather than ellipsizing it here: there is still somewhere to put those
      // characters, and a device secret or a hostname half-shown is still half-readable.
      size_t fitted = 0;
      for (size_t take = 1; cursor + take <= length && take <= capacity; ++take) {
        if (isBreak(source[cursor + take - 1])) {
          break;
        }
        memcpy(line, source + cursor, take);
        line[take] = '\0';
        if (lcd.textWidth(line) > maxWidth) {
          break;
        }
        fitted = take;
      }
      lineEnd = cursor + (fitted == 0 ? 1 : fitted);
    }

    size_t rest = lineEnd;
    while (rest < length && isBreak(source[rest])) {
      ++rest;
    }
    const bool lastLine = (linesDrawn + 1 >= maxLines);

    if (rest < length && lastLine) {
      size_t take = lineEnd - cursor;
      if (take > capacity - 4) {
        take = capacity - 4;
      }
      while (true) {
        while (take > 0 && isBreak(source[cursor + take - 1])) {
          --take;
        }
        for (size_t i = 0; i < take; ++i) {
          line[i] = isBreak(source[cursor + i]) ? ' ' : source[cursor + i];
        }
        line[take] = '\0';
        strcat(line, "...");
        if (take == 0 || lcd.textWidth(line) <= maxWidth) {
          break;
        }
        --take;
      }
      Journal::printf("[display] wrapped text ran out of lines, %u characters ellipsized",
                      static_cast<unsigned>(length - (cursor + take)));
      cursor = length;
    } else {
      for (size_t i = cursor; i < lineEnd; ++i) {
        line[i - cursor] = isBreak(source[i]) ? ' ' : source[i];
      }
      line[lineEnd - cursor] = '\0';
      cursor = lineEnd;
    }

    lcd.drawString(line, kScreenW / 2, cursorY);
    cursorY += lineHeight;
    linesDrawn++;
  }

  return linesDrawn;
}

}  // namespace

void begin() {
  // Still means exactly what it always meant - "light the panel now" - and is
  // still called from the top of CAL's ladder on every boot that is going to
  // draw anything. What changed is that it is no longer the only thing that
  // can bring the panel up; see startPanel() above.
  startPanel();
}

void showNeutralSplash() {
  startPanel();
  clear();
  centeredText("Discover", 70, kInk, 4);
  centeredText("Around Me", 110, kAccent, 4);
  centeredText("starting", 170, kMuted, 2);
}

// The row-streaming read shared by showBrandSplash() (the standalone,
// centred boot splash) and drawSplashBackdrop() below (the same image, moved
// higher to leave room underneath for status text) - one definition of "how
// to get /brand.565 onto the panel" rather than two copies that could drift
// on the row format or the truncated-file guard. Does not clear() - callers
// decide that, since drawSplashBackdrop() needs the screen cleared before
// this runs but showBrandSplash() needs it cleared at exactly the same point
// it always was.
bool drawBrandImage(int y0) {
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

  // Streamed a row at a time. A full-screen buffer would be 150KB and this
  // device has no PSRAM; one row is 480 bytes.
  static uint16_t row[kBrandW];
  const int x0 = (kScreenW - kBrandW) / 2;
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

// The backdrop every showStatus() call draws behind its text, so the several
// seconds of "Looking for known networks" / "Contacting service" / etc. a
// boot works through read as one continuous screen with a status line
// updating on it, rather than the brand identity flashing once at the very
// start and then vanishing behind blank-and-retype text for the rest of the
// boot. Positioned higher (y0=20, so the 120px-tall image ends at 140) than
// the standalone showBrandSplash()'s y0=40 specifically to leave the ~100px
// below it that showStatus()'s text needs - the two callers want the same
// image at different heights for different reasons, which is why this takes
// y0 as a parameter instead of both reusing one fixed constant.
void drawSplashBackdrop() {
  clear();
  if (!drawBrandImage(20)) {
    // No brand asset yet (a unit not yet authenticated, or one whose brand
    // never uploaded one) - the identical two-line wordmark
    // showNeutralSplash() uses, just smaller and higher, for the same
    // leave-room-below reason as the branded case above.
    centeredText("Discover", 45, kInk, 3);
    centeredText("Around Me", 80, kAccent, 3);
  }
}

void showStatus(const String& headline, const String& detail) {
  startPanel();
  drawSplashBackdrop();
  // Both wrapped, not just positioned with fixed offsets: headline in particular
  // can be a server-supplied sentence (see the enrollment "waiting"/"refused"
  // messages) with no length CAL controls, and detail's start position follows
  // however many lines the headline actually needed rather than assuming one.
  // Capped at 2 lines (was 3) - the backdrop above now claims the screen's top
  // ~140px, leaving less room below for text than the blank-screen version
  // this replaces had.
  const int headlineLines = wrappedCenteredText(headline, 155, kInk, 2, 22, 2);
  if (detail.length() > 0) {
    wrappedCenteredText(detail, 155 + headlineLines * 22 + 12, kMuted, 1, 14, 2);
  }
}

bool showBrandSplash() {
  startPanel();
  clear();
  return drawBrandImage(40);
}

void showFailure(const String& headline, const String& whatToDo) {
  startPanel();
  clear();
  const int headlineLines = wrappedCenteredText(headline, 75, kWarn, 2, 22, 3);
  wrappedCenteredText(whatToDo, 75 + headlineLines * 22 + 12, kMuted, 1, 14, 3);
}

void showQr(const String& url, const String& caption, const String& subCaption) {
  startPanel();
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
  y += wrappedCenteredText(caption, y, kInk, 2, 22, 2) * 22;
  if (subCaption.length() > 0) {
    y += wrappedCenteredText(subCaption, y, kAccent, 2, 22, 2) * 22;
  }
  // The address in characters as well as in the code, because cameras fail.
  wrappedCenteredText(url, y, kMuted, 1, 14, 2);
}

void showUpdateProgress(uint8_t percent, const String& version) {
  startPanel();
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
