// sdkconfig.h first, or CONFIG_IDF_TARGET_ESP32S3 is undefined when the board gate
// below is evaluated and an S3 build silently takes the autodetect branch. That
// cost a flash: the Panellan pin map was absent from a binary that compiled
// clean, because nothing outside the header referenced it.
#include "sdkconfig.h"
// The board is chosen here and nowhere else. Everything below this block, and
// every drawing call in this file, is the same on both boards.
//
// CONFIG_IDF_TARGET_ESP32S3 comes from the SDK, so it follows the FQBN without
// anything having to pass it in. Only the taken branch compiles, which means a
// break in the other one stays invisible until somebody builds for that board -
// CI has to build BOTH or the untaken branch rots.
//
// The pin map comes from the DamBoards library, not from this folder, so CAL and
// App read one copy of it. The compile scripts put that library on the search
// path with --libraries.
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#include <LovyanGFX.hpp>
#include <BoardPanellan.h>
#else
#define LGFX_AUTODETECT
#include <LovyanGFX.hpp>
#include <LGFX_AUTODETECT.hpp>
#endif

#include <FS.h>
#include <LittleFS.h>

// Vendored copy of ricmoo/QRCode, renamed. A plain <qrcode.h> resolves to the
// ESP32 core's own esp_qrcode header instead of the library, which fails at
// compile time with the core's differently-named API suggested in its place.
// Renaming both the file and its include guard is what makes the local copy
// win; the functions inside are untouched and do not clash with esp_qrcode_*.
#include "CalQr.h"

#include "Display.h"
#include <PresenceInput.h>
// wrappedCenteredText() says when it has ellipsized. A boot screen whose
// message was cut is exactly the kind of thing nobody notices until a
// household reads half an instruction, and the journal survives the reboot
// that Serial does not.
#include "Journal.h"
#include <string.h>

namespace Display {
namespace {

// One declaration, two definitions of LGFX, picked by the block at the top of
// this file.
//
// On the ESP32-2432S028, LGFX_AUTODETECT senses the board rather than requiring
// a pin map. The panel there is an LCDWIKI E32R28T whose HSPI-style pinout
// differs from the Sunton boards most published pin maps describe, and
// autodetect gets it right where a copied pin map does not.
//
// On the Panellan, BoardPanellan.h supplies the pin map by hand, because
// LovyanGFX has no entry for that panel to detect.
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

// White, for the one screen that inverts. Named rather than written inline so the
// page-style guard and anybody reading the palette can see there are two grounds.
constexpr uint32_t kPaper = 0xFFFFFFu;

// Near-black, for text on kPaper. Not pure black: a hair of blue sits better against
// a ground this panel renders slightly blue anyway.
constexpr uint32_t kSlate = 0x101828u;

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

#if defined(CONFIG_IDF_TARGET_ESP32S3)
  // Backlight driven directly, before the driver touches it. On this panel
  // nothing is visible until GPIO47 is high, and a Light_PWM whose output never
  // reaches the pin is indistinguishable from a bus that never reaches the
  // glass: both are a black screen with a clean init behind them. Asserting it
  // here separates the two, and costs one pin write on a board whose backlight
  // is wanted whenever the panel is.
  pinMode(panellan::kBacklight, OUTPUT);
  digitalWrite(panellan::kBacklight, HIGH);
#endif

  const bool panelReady = lcd.init();
  lcd.setRotation(1);  // 320x240 landscape
  lcd.setBrightness(255);

  // init() false means the driver could not bring the panel up at all. The
  // dimensions say whether rotation landed where the card layouts expect.
  Journal::printf("[display] panel init=%d %ux%u",
                  panelReady ? 1 : 0,
                  static_cast<unsigned>(lcd.width()),
                  static_cast<unsigned>(lcd.height()));
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

// background defaults to kBg, because every caller but one draws on the dark panel.
// It is a parameter at all because showConnected draws on kPaper, and this function
// used to fill each glyph cell with kBg regardless - which on a white screen painted
// the text as solid black bars. Seen on a real unit 2026-10-24.
int wrappedCenteredText(const String& text, int y, uint32_t colour, uint8_t size,
                        int lineHeight, int maxLines, uint32_t background = kBg) {
  lcd.setTextColor(colour, background);
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

// THE ONE SCREEN DRAWN DARK-ON-LIGHT, and it is deliberate. Every other screen
// here is light text on the product's near-black panel. This one inverts so that
// a household glancing up from their phone can tell at a distance that the join
// worked, without reading a word of it.
//
// Shown while a phone is associated to the setup access point. Provisioning swaps
// back to the join code the moment the last station leaves, so the screen always
// says which of the two things the person should be doing.
// LovyanGFX already drives touch; Recovery adds the gesture and release tracking.
bool readTouch(int32_t& x, int32_t& y) {
  return gPanelStarted && lcd.getTouch(&x, &y);
}

void showRecoveryMenu(Recovery::Screen screen) {
  startPanel();
  clear();
  const auto button = [](const Recovery::Rect& rect, const char* label, uint32_t colour) {
    lcd.drawRoundRect(rect.x, rect.y, rect.w, rect.h, 5, colour);
    lcd.setTextDatum(middle_center);
    lcd.setTextSize(2);
    lcd.setTextColor(colour, kBg);
    lcd.drawString(label, rect.x + rect.w / 2, rect.y + rect.h / 2);
  };
  if (screen == Recovery::Screen::Menu) {
    centeredText("Configuration", 12, kInk, 2);
    button(Recovery::kMenuRows[0], "Set up Wi-Fi", kInk);
    button(Recovery::kMenuRows[1], "Reset identity", kWarn);
    button(Recovery::kMenuRows[2], "Reload application", kInk);
    button(Recovery::kPresenceTest, "Diagnostics", kInk);
    button(Recovery::kBack, "Back", kMuted);
    return;
  }
  if (screen == Recovery::Screen::Diagnostics) {
    centeredText("Diagnostics", 12, kInk, 2);
    button(Recovery::kMenuRows[0], "Presence sensor", kInk);
    button(Recovery::kMenuRows[1], "GPIO monitor", kInk);
    button(Recovery::kMenuRows[2], "Back", kMuted);
    return;
  }
  const bool identity = screen == Recovery::Screen::ConfirmIdentity;
  centeredText(identity ? "Reset identity?" : "Reload application?", 18, kWarn, 2);
  wrappedCenteredText(identity ? "Removes this device's secret. An administrator may need to allow re-registration."
                               : "Downloads the current application again, even if its version matches. Keep power on.",
                      58, kInk, 2, 20, 5);
  button(Recovery::kCancel, "Cancel", kMuted);
  button(Recovery::kConfirm, identity ? "Reset" : "Reload", kWarn);
}

void showPresenceTest(const PresenceTest::State& reading, uint32_t now, bool supported,
                      bool redrawFrame) {
  startPanel();
  if (redrawFrame) {
    clear();
    centeredText("Presence sensor", 10, kInk, 2);
  }
  lcd.fillRect(0, 38, kScreenW, 20, kBg);
  lcd.fillRect(42, 70, 236, 14, kBg);
  lcd.fillRect(0, 94, kScreenW, 64, kBg);
  constexpr uint32_t green = 0x38C878u;
  const uint32_t signalColour = supported && reading.high ? green : kMuted;
  centeredText(!supported ? "No sensor input mapped"
                         : !reading.hasReading ? "Waiting for reading"
                                               : reading.high ? "Signal HIGH" : "Signal LOW",
               40, signalColour, 2);
  // Same geometry as CAL's download progress bar; binary output, not distance
  // or a confidence percentage. Unsupported boards show an unfilled outline.
  lcd.drawRect(40, 68, 240, 18, kMuted);
  if (supported && reading.hasReading && reading.high) {
    lcd.fillRect(42, 70, 236, 14, green);
  }
  if (supported && reading.hasReading) {
    centeredText("GPIO35: " + String(reading.mv) + " mV", 96, kInk, 2);
    centeredText("Signal changes: " + String(reading.changes), 120, kInk, 1);
    centeredText(reading.changes ? "Last change: " + String((now - reading.lastChangeMs) / 1000) + " s ago"
                                : String("Last change: none yet"), 134, kMuted, 1);
    centeredText(now < PresenceInput::kStabilizeMs ? "Power-up settling; raw signal only" : "Raw signal test; App policy not loaded",
                 148, kMuted, 1);
    for (uint8_t i = 0; i < PresenceTest::kHistorySize; ++i) {
      const uint8_t value = reading.historyAt(i);
      lcd.fillRect(40 + i * 5, 164, 4, 10, value == 2 ? green : value == 1 ? kMuted : 0x202020u);
    }
    centeredText("Recent signal: last 12 seconds", 177, kMuted, 1);
  } else if (!supported) {
    centeredText("Panellan ESP32-S3", 101, kInk, 2);
    centeredText("No fixed PIR pin is mapped.", 131, kMuted, 1);
    centeredText("Use GPIO monitor for wired inputs.", 149, kMuted, 1);
  }
  if (redrawFrame) {
    const Recovery::Rect& back = Recovery::kPresenceBack;
    lcd.drawRoundRect(back.x, back.y, back.w, back.h, 5, kMuted);
    centeredText("Back", back.y + 13, kInk, 2);
  }
}

void showGpioMonitor(const GpioMonitor::State& state, bool allPins, uint8_t page,
                     uint32_t now, bool redrawFrame) {
  startPanel();
  const auto label = [](const char* text, int x, int y, uint8_t size, uint32_t colour) {
    lcd.setTextDatum(top_left); lcd.setTextSize(size); lcd.setTextColor(colour, kBg);
    lcd.drawString(text, x, y);
  };
  const auto button = [&](const Recovery::Rect& r, const char* text) {
    lcd.drawRoundRect(r.x, r.y, r.w, r.h, 4, kMuted);
    label(text, r.x + 9, r.y + 15, 1, kInk);
  };
  if (redrawFrame) {
    clear();
    label("GPIO monitor", 16, 10, 2, kInk);
    lcd.drawRoundRect(224, 4, 80, 28, 4, kMuted);
    label(allPins ? "Changes" : "All pins", 233, 14, 1, kInk);
    button(Recovery::kGpioPrevious, "Previous");
    button(Recovery::kGpioBack, "Back");
    button(Recovery::kGpioNext, "Next");
  }
  lcd.fillRect(0, 32, kScreenW, 152, kBg);
  char text[64];
  snprintf(text, sizeof(text), allPins ? "All GPIO numbers - page %u/%u"
                                     : "Watching %u inputs - changes %u/2",
           allPins ? page + 1 : GpioMonitor::kInputCount,
           allPins ? (GpioMonitor::kPortCount + 3) / 4 : page + 1);
  label(text, 16, 32, 1, kMuted);
  for (uint8_t row = 0; row < 4; ++row) {
    const uint8_t index = page * 4 + row;
    uint8_t pin = index;
    uint32_t value = 0, changedMs = 0, changes = 0;
    bool sampled = false, high = false, changed = false;
    if (allPins) {
      if (pin >= GpioMonitor::kPortCount) break;
      const GpioMonitor::Input* input = state.input(pin);
      if (input) {
        value = input->value; sampled = input->sampled; high = input->high;
        changedMs = input->lastChangeMs; changes = input->changes; changed = changes > 0;
      }
    } else {
      if (index >= state.count()) {
        if (row == 0) label("Waiting for signal changes", 16, 60, 1, kMuted);
        break;
      }
      const GpioMonitor::Event& event = state.event(index);
      pin = event.pin; value = event.value; high = event.high; sampled = true;
      changedMs = event.when; changed = true; changes = state.input(pin)->changes;
    }
    const int y = 50 + row * 33;
    const uint32_t colour = changed && static_cast<uint32_t>(now - changedMs) < 2000 ? 0x38C878u : kInk;
    snprintf(text, sizeof(text), "GPIO %u", pin);
    label(text, 16, y, 2, colour);
    const GpioMonitor::Kind kind = GpioMonitor::kind(pin);
    if (sampled) {
      if (kind == GpioMonitor::Kind::Analog) snprintf(text, sizeof(text), "%lumV %s", static_cast<unsigned long>(value), high ? "HIGH" : "LOW");
      else snprintf(text, sizeof(text), "%s", high ? "HIGH" : "LOW");
      label(text, 118, y + 3, 1, colour);
      lcd.drawRect(216, y + 1, 88, 12, kMuted);
      const uint32_t level = kind == GpioMonitor::Kind::Analog ? (value > 3300 ? 3300 : value) : high ? 3300 : 0;
      lcd.fillRect(218, y + 3, (84 * level) / 3300, 8, colour);
      if (changed) snprintf(text, sizeof(text), "Changes %lu - last %lus ago", static_cast<unsigned long>(changes), static_cast<unsigned long>((now - changedMs) / 1000));
      else snprintf(text, sizeof(text), "Baseline - no change yet");
    } else {
      snprintf(text, sizeof(text), "%s", kind == GpioMonitor::Kind::Unavailable ? "Unavailable on this chip" : "Reserved/unverified - not sampled");
    }
    label(text, 16, y + 20, 1, kMuted);
  }
}

void showConnected(const String& headline, const String& detail) {
  startPanel();
  lcd.fillScreen(kPaper);

  // kInk is white and would vanish here. Both lines are the accent blue, the
  // detail a size smaller rather than a lighter colour, because there is no
  // muted tone that stays legible on white at size 1 on this panel.
  // kSlate, not kAccent. The panel carries a blue cast - kBg is pure black and
  // photographs as deep blue - so white comes out pale blue and accent-blue text on
  // it is blue on blue. Near-black against that ground is a far wider gap, and this
  // is the screen somebody reads at arm's length across a room.
  const int headlineLines = wrappedCenteredText(headline, 80, kSlate, 3, 30, 2, kPaper);
  if (detail.length() > 0) {
    wrappedCenteredText(detail, 80 + headlineLines * 30 + 16, kSlate, 2, 22, 3, kPaper);
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

void showQr(const String& url, const String& caption, const String& subCaption,
            const String& fallbackText) {
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
  // THREE LINES WHEN NOTHING FOLLOWS, TWO WHEN SOMETHING DOES. The enrollment
  // screen drew "Waiting for an administrator to set t..." on 2026-10-24: the
  // server writes that message and a two-line cap cut it mid-word. The screen
  // has room for a third line only when there is no subcaption under it, so the
  // cap is computed rather than raised, and nothing can run off the panel.
  const int captionLines = subCaption.length() > 0 ? 2 : 3;
  y += wrappedCenteredText(caption, y, kInk, 2, 22, captionLines) * 22;
  if (subCaption.length() > 0) {
    // The network name, which somebody matches against a list on a phone.
    // kAccent is blue and this panel is blue: a photograph of a real unit on
    // 2026-10-23 showed the name, and the address below it, as the two least
    // readable things on a screen whose whole job is to be read off.
    y += wrappedCenteredText(subCaption, y, kInk, 2, 22, 2) * 22;
  }
  // The Wi-Fi QR payload only joins a network; the setup URL is separate.
  // For every other caller keep showing the actual QR payload as before.
  //
  // kInk, not kMuted. This is the fallback for a phone that does not open the
  // portal by itself, so it is the one thing here somebody types by hand, and
  // grey on blue at size 1 is not typeable.
  wrappedCenteredText(fallbackText.length() > 0 ? fallbackText : url,
                      y, kInk, 1, 14, 2);
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
