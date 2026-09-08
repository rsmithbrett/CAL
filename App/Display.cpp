// SD.h MUST come before LovyanGFX.hpp, not after. LovyanGFX auto-detects
// SD-card image support by checking whether the SD library's own include
// guard is already defined (see its esp32/common.hpp); include it afterwards
// and drawPngFile(SD, ...) fails to compile with "abstract type
// DataWrapperT<fs::SDFS>". CYD-Dickey hit exactly this and records the same
// note at the top of its .ino.
#include <SD.h>
#include <memory>

#define LGFX_AUTODETECT
#include <LovyanGFX.hpp>
#include <LGFX_AUTODETECT.hpp>
#include <time.h>

// Vendored copy of ricmoo/QRCode, renamed - identical copy and identical reasoning as
// CAL's own root Display.cpp: a plain <qrcode.h> resolves to the ESP32 core's own
// esp_qrcode header instead of the library, which fails at compile time with the core's
// differently-named API suggested in its place. Renaming both the file and its include
// guard is what makes the local copy win; the functions inside are untouched and do not
// clash with esp_qrcode_*. This is a second copy of the same two files (CalQr.h/.c),
// vendored into App/ rather than referenced across sketch directories - the Arduino build
// model compiles each sketch folder independently, so App needs its own copy exactly the
// way it already has its own Display.cpp rather than sharing CAL's.
#include "CalQr.h"

#include "Display.h"
#include "Log.h"

namespace Display {
namespace {

// Same panel as CAL - see CAL's own Display.cpp for why autodetect over a pin map.
LGFX lcd;

constexpr int kScreenW = 320;
constexpr int kScreenH = 240;

// Day/night theme (see Display.h's own remarks on setEnvironment() and the
// note at the bottom of that file): every screen this file draws - the
// boot-ladder screens below and the content-card family further down alike
// - picks one of these two pairs via bg()/ink()/muted(), rather than each
// screen family owning its own fixed palette the way this restyle's first
// pass left them. "Day" is exactly CYD-Dickey's white-card look this file
// already had; "night" is exactly the black boot-ladder look this file
// already had - this is a wholesale swap between two looks that both
// already existed here, not a new invention either way.
constexpr uint32_t kBgDay = 0xFFFFFFu;
constexpr uint32_t kInkDay = 0x000000u;
constexpr uint32_t kMutedDay = 0x707070u;
constexpr uint32_t kBgNight = 0x000000u;
constexpr uint32_t kInkNight = 0xFFFFFFu;
constexpr uint32_t kMutedNight = 0x9A9A9Au;
// Deliberately outside the day/night swap - see the note at the bottom of
// Display.h for why: amber already reads against either background, and a
// fixed white banner label needs to stay white regardless of theme since
// the banner rects below (kForecastBanner/kAircraftBanner) are their own
// fixed dark colour blocks, not part of the swap either.
constexpr uint32_t kWarn = 0xEDA100u;
constexpr uint32_t kBannerLabelInk = 0xFFFFFFu;

// Set by setEnvironment(), read by bg()/ink()/muted()/drawClock() below.
// Defaults match App.ino's own pre-first-check-in defaults (0 minutes,
// daytime) so the very first boot screens - drawn before any check-in has
// ever completed - still render sensibly.
int gUtcOffsetMinutes = 0;
bool gIsDaytime = true;

uint32_t bg() { return gIsDaytime ? kBgDay : kBgNight; }
uint32_t ink() { return gIsDaytime ? kInkDay : kInkNight; }
uint32_t muted() { return gIsDaytime ? kMutedDay : kMutedNight; }

constexpr uint32_t kAircraftBanner = 0x1F6FEBu;
// Amber, distinct from the two blues above so the three cards are told apart at
// a glance from across a room rather than by reading the banner text.
constexpr uint32_t kSunMoonBanner = 0xB45309u;
// A deep indigo, distinct from every banner above it - this is the first
// "graphical style" card (an illustration rather than a data reading or
// prose), so it earns a colour family of its own rather than reusing sun's
// amber just because the two share an astronomy source.
constexpr uint32_t kMoonPhaseBanner = 0x4B2E83u;
// A muted green, distinct from every banner above it - this card is the only
// one whose entire content is server-chosen prose rather than a data reading,
// so it gets a colour that reads as neither "weather" nor "aircraft" nor
// "sunrise" at a glance.
constexpr uint32_t kAnnouncementBanner = 0x2E7D32u;
// Teal, distinct from every banner above it - reads as "scan this" at a glance rather than
// as any of weather/aircraft's blues, sun's amber, moon's indigo or the notice's green.
constexpr uint32_t kQrTextBanner = 0x0E7490u;
// A brick/terracotta red, distinct from every banner above it - real-estate
// signage colour rather than anything borrowed from the weather/aircraft/
// astronomy families this card has nothing to do with.
constexpr uint32_t kListingsBanner = 0xA13D2Du;
// A deep sea-teal, distinct from both weather/aircraft's brighter blues and
// QR's cyan-teal despite sharing the same colour family - tides is the one
// card whose content is genuinely ocean data, so it earns the "ocean" hue,
// just a darker, greener shade than either of the two colours already in use
// so the three are still told apart at a glance.
constexpr uint32_t kTidesBanner = 0x0F6B5Cu;
// Near-black indigo, distinct from every banner above it including moon's
// lighter indigo - the one card whose subject is literally outer space, so
// it earns the darkest banner on the device rather than competing for a hue
// already claimed by an astronomy sibling.
constexpr uint32_t kIssFlyoverBanner = 0x1A1A2Eu;
// A plum/violet, distinct from every banner above it - closest in family to
// moon's indigo, but far enough apart (magenta-leaning vs. blue-leaning) that
// the two are told apart at a glance from across a room, matching how
// weather's navy and aircraft's bright blue already coexist as the two
// closest-related pair on this device.
constexpr uint32_t kForecastBanner = 0x6A1B9Au;
constexpr int kBannerHeight = 22;
constexpr int kCardMargin = 10;

// Card chrome geometry - see Display.h's own remarks on why all of it is
// decided here rather than described by the server.
//
// The button row has to stay clear of two things vertically: the card
// content above it (a four-line forecast plus its "updated" line already
// reaches roughly y=186 - see drawClock()'s own remarks on why the clock
// itself is stuck at 9pt) and the corner clock below, which drawClock() sets
// bottom-right at (314, 236). The clock was enlarged to FreeSans9pt after it
// proved invisible on real hardware at its original 6x8 bitmap size, so it
// now occupies roughly y 222-236, x 265-314. kButtonHeight doubled (30 to
// 60) after real fingers found the original height hard to hit reliably -
// same motivation as the width increase the next paragraph already
// documents, just the other axis. The row's bottom edge stays pinned at
// y=220 (still clearing the clock by 2px); the extra 30px comes out of the
// top, moving kButtonRowY from 190 to 160 - which is why
// showAnnouncementCard() and showQrTextCard() below both had their own
// content budgets re-checked against the new boundary rather than left
// assuming the old one.
//
// Width is a different story. The touch controller checks action-button
// zones before the reverse/forward edge strips (see Touch.cpp's poll() and
// its own remarks on why that ordering is fixed) - a tap landing inside a
// button rect is always a button press, regardless of how close that rect
// sits to the physical edge. So unlike the (now ~106px) edge strips
// themselves, which have to stay clear of card content in the middle of the
// screen, a button row has nothing logical to stay clear of horizontally,
// and can run almost the full 320px width. Real fingers found the old
// 24px-to-296px row (272px total, narrowing to 85px for a three-button card)
// too narrow to hit reliably; it now runs 8px to 312px (304px total, 97px
// for three buttons - roughly +14% per button), with a small residual
// margin from the true bezel edge kept only because a resistive panel's
// accuracy is known to degrade right at the glass edge, not because
// anything would misfire.
constexpr int kButtonRowY = 160;
constexpr int kButtonHeight = 60;
constexpr int kButtonRowLeft = 8;
constexpr int kButtonRowRight = 312;
constexpr int kButtonGap = 6;
constexpr int kButtonRadius = 6;

// The same bright, high-contrast blue CYD-Dickey settled on for its own
// buttons (its BUTTON_COLOR = 0x2E9FFF), chosen there because the default
// dark navy was hard to read on this panel. Deliberately outside the
// day/night swap for the same reason the banner colours are: it reads
// against either background, and white-on-blue stays legible either way.
constexpr uint32_t kButtonFill = 0x2E9FFFu;
constexpr uint32_t kButtonPressedFill = 0x0B5FB0u;
constexpr uint32_t kButtonInk = 0xFFFFFFu;

// showButtonPressConfirmation()'s big centre-screen acknowledgment. Same
// green as kAnnouncementBanner - "confirmed" reuses the one colour on this
// panel that already reads as affirmative rather than introducing a second
// green with a different meaning. Deliberately outside the day/night swap,
// same reasoning as the button colours above: it needs to read clearly
// against whichever background happens to be showing underneath it.
constexpr uint32_t kConfirmFill = 0x2E7D32u;
constexpr uint32_t kConfirmInk = 0xFFFFFFu;

// Edge chevrons. Small, low-contrast, vertically centred - they mark the
// touch zones without competing with the card for attention.
constexpr int kChevronHalfHeight = 12;
constexpr int kChevronWidth = 7;
constexpr int kChevronCentreY = kScreenH / 2;

// Mirrors Touch.cpp's own kEdgeZoneWidth exactly (see that file's remarks on
// why 106, not the original 16). Duplicated rather than shared for the same
// reason kScreenW here and kScreenWidth there are already duplicated instead
// of one file including the other's constant: Touch stays ignorant of
// Display and Display stays ignorant of Touch's hit-testing internals.
// flashNavEdge() below needs this to fill exactly the rect Touch::poll()
// reads taps from, so if one of these two numbers ever changes without the
// other, a flash would light up a strip narrower or wider than the zone that
// actually responds to a finger.
constexpr int kEdgeZoneWidth = 106;

void clear() {
  lcd.fillScreen(bg());
}

// Bottom-right corner clock, drawn by every card-rendering function further
// down (see Display.h's remarks on setEnvironment()) regardless of that
// card's Ok/error state - a clock is chrome, not content, and shouldn't
// disappear just because a card is showing a problem. Local time is UTC
// (already synchronised over SNTP - see AppService::synchroniseTime()) plus
// the check-in-supplied offset; DST is already folded into that offset
// server-side (see CheckIn.h's own remarks), so no DST math happens here.
// Uses the same time_t -> tm idiom as CheckIn.cpp's nowAsIso8601Utc()
// (gmtime_r on a shifted time_t, rather than reaching for localtime() and a
// TZ this firmware never sets).
//
// Redrawn only when the card underneath it redraws (content refresh, a
// forced update check, or now a touch tap - see Touch.h) rather than on an
// independent per-second ticker: none of this file's draw functions are
// called more often than that today, and adding a ticking redraw path for a
// corner clock that already updates on every card refresh was judged not
// worth the added complexity - see README.
void drawClock() {
  const time_t localNow = time(nullptr) + static_cast<time_t>(gUtcOffsetMinutes) * 60;
  struct tm localTm;
  gmtime_r(&localNow, &localTm);
  char buffer[6];
  snprintf(buffer, sizeof(buffer), "%02d:%02d", localTm.tm_hour, localTm.tm_min);

  // Sized for a person across a room, not for a screenshot. The first version of
  // this used Font0 at size 1 in muted grey - 6x8 pixels per character, roughly
  // 3mm tall on this 2.8" panel, grey on white - and the first person to see it on
  // real hardware reported there was no clock at all. It was drawing correctly the
  // whole time; it simply could not be seen, which for a display whose entire job
  // is being read from a distance is the same thing as not working.
  //
  // FreeSansBold at size 1 is about 13px tall here, and ink() rather than
  // muted() keeps it legible in both themes. Still corner chrome - it must not
  // compete with the card - but chrome you can actually read.
  //
  // 9pt rather than the 12pt the temperature uses is a hard constraint, not a
  // preference: the button row above ends at y=220 and the clock's baseline sits
  // at y=236, so there are 16 pixels to work in. 12pt bold needs about 17 and
  // would collide. Going bigger means moving the button row up, and the row
  // cannot move up without reflowing the card body above it - a four-line
  // forecast plus its "updated" line already reaches roughly y=186. So a bigger
  // clock is a card-layout change, not a font change.
  lcd.setFont(&fonts::FreeSansBold9pt7b);
  lcd.setTextSize(1);
  lcd.setTextColor(ink(), bg());
  lcd.setTextDatum(bottom_right);
  lcd.drawString(buffer, kScreenW - 6, kScreenH - 4);
}

// The boot-ladder screens' (showStatus/showFailure) own word-wrap - greedy,
// measured with real font metrics, since server-supplied strings (a card's
// shortForecast, a content-gate refusal message) arrive with no length this
// file controls. See wrappedLeftText further down for the card-layout
// counterpart this restyle adds alongside it.
int wrappedCenteredText(const String& text, int y, uint32_t colour, uint8_t size,
                        int lineHeight, int maxLines) {
  lcd.setTextColor(colour, bg());
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

// Same greedy word-wrap as wrappedCenteredText above, but left-margined at x
// instead of centered across the whole screen width - the card layout this
// restyle borrows from CYD-Dickey lays out every card against a fixed left
// margin (their drawWeatherCard()/drawFeaturedAircraft() both use a plain
// lcd.setCursor(10, ...) column), not centered text. Kept as a separate
// function rather than adding an alignment flag to wrappedCenteredText:
// showStatus()/showFailure() above are the shared boot-ladder surface
// WifiJoin and CAL's own Provisioning module assume renders centered, and
// this restyle doesn't touch that.
//
// measureOnly runs the identical wrap and returns the identical line count
// without putting anything on screen, so a caller can ask "how tall would
// this be at the font I currently have selected?" and choose a size before
// committing to it - see showWeatherCard(), which uses it to keep a long
// forecast phrase whole at a smaller size rather than clipping it at a
// larger one. Deliberately the same function rather than a parallel
// measuring one, because a measurement that can drift from the drawing it
// predicts is worse than no measurement at all.
int wrappedLeftText(const String& text, int x, int y, uint32_t colour, int lineHeight,
                    int maxLines, int maxWidth, bool measureOnly = false) {
  lcd.setTextColor(colour, bg());
  lcd.setTextDatum(top_left);

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
        if (!measureOnly) {
          lcd.drawString(candidate, x, cursorY);
        }
        linesDrawn++;
      }
      continue;
    }

    const int breakAt = (lastSpace > lineStart) ? lastSpace : i;
    if (!measureOnly) {
      lcd.drawString(text.substring(lineStart, breakAt), x, cursorY);
    }
    cursorY += lineHeight;
    linesDrawn++;
    lineStart = (lastSpace > lineStart) ? lastSpace + 1 : breakAt;
    lastSpace = -1;
    i = lineStart - 1;
  }

  return linesDrawn;
}

// Small colour-banded label in the top-left corner, e.g. CYD-Dickey's
// drawWeatherCard() doing `lcd.fillRect(0, 0, 110, 22, TFT_NAVY)` then
// printing "WEATHER" in bold white on top of it - every one of their cards
// (weather, listings, QR, branding) opens the same way, just with a
// different fixed width/colour/label. width is per-card because the label
// text itself varies ("WEATHER" vs "OVERHEAD").
void drawCardBanner(const String& label, uint32_t bannerColour, int width) {
  lcd.fillRect(0, 0, width, kBannerHeight, bannerColour);
  lcd.setFont(&fonts::FreeSansBold9pt7b);
  lcd.setTextSize(1);  // GFX fonts are sized at their own point size - see drawCardBanner's callers
  lcd.setTextColor(kBannerLabelInk, bannerColour);
  lcd.setTextDatum(top_left);
  lcd.drawString(label, 8, 4);
}

// Every card-drawing function below switches to a bold sans GFX font
// (fonts::FreeSansBold*); this puts the default bitmap font back before
// returning, matching CYD-Dickey's own explicit `lcd.setFont(&fonts::Font0);
// // restore the default -- other screens assume it` at the end of each of
// its draw functions. Without this, showStatus()/showFailure() above -
// which never call setFont themselves and assume whatever the default is -
// would render in whatever bold font the last card left selected.
void restoreDefaultFont() {
  lcd.setFont(&fonts::Font0);
  lcd.setTextDatum(top_left);
}

// Right-justifies text against rightX, truncating one character at a time
// until it fits maxWidthPx - identical technique to CYD-Dickey's
// drawTruncatedRight(), used there so a short value ("225kts") and a long
// one ("British Airways") both end flush at the same right margin instead of
// starting ragged from the left. Assumes the caller already set font/size/
// colour, same as their version.
void drawRightJustified(String text, int rightX, int y, int maxWidthPx) {
  while (text.length() > 1 && lcd.textWidth(text) > maxWidthPx) {
    text = text.substring(0, text.length() - 1);
  }
  lcd.setTextDatum(top_right);
  lcd.drawString(text, rightX, y);
  lcd.setTextDatum(top_left);
}

// Left-margined counterpart to drawRightJustified above: same
// truncate-one-character-at-a-time technique, anchored at the left instead.
// Used for the weather card's location and freshness lines, both of which are
// short in practice but come from data this file does not control (a city
// name the owner typed, say), and neither of which may be allowed to run off
// the right edge of the panel.
void drawTruncatedLeft(String text, int x, int y, int maxWidthPx) {
  while (text.length() > 1 && lcd.textWidth(text) > maxWidthPx) {
    text = text.substring(0, text.length() - 1);
  }
  lcd.setTextDatum(top_left);
  lcd.drawString(text, x, y);
}

// The hand-drawn-degree-ring technique from the original centeredTemperature
// above, adapted to a left-aligned origin instead of screen-centered - this
// restyle's cards lay out left-margined (see wrappedLeftText's remarks), so
// the temperature moves to match rather than staying centered on its own.
// The bold GFX font used by the caller is exactly as ASCII-only as the bitmap
// font the original comment describes - the missing-glyph problem, and the
// reason for drawing this ring instead of a literal degree character, applies
// here unchanged.
//
// ringRadius is a parameter rather than a constant because the ring has to
// scale with whatever font the caller selected: a fixed 5px ring that looked
// like a degree mark beside a 12pt numeral looks like a stray speck beside a
// 24pt one. The gap either side scales with it for the same reason.
void drawTemperature(int temperature, const String& unit, int x, int y, uint32_t colour,
                     int ringRadius) {
  lcd.setTextColor(colour, bg());
  lcd.setTextDatum(top_left);

  const String numberText = String(temperature);
  int cursorX = x;
  lcd.drawString(numberText, cursorX, y);
  cursorX += lcd.textWidth(numberText);

  const int gap = ringRadius;
  cursorX += gap;
  // Near the top of the glyph's cap-height, like a real superscript degree
  // mark - same placement rationale as the original, just against this
  // font's taller cap-height.
  const int ringCentreX = cursorX + ringRadius;
  const int ringCentreY = y + ringRadius;
  lcd.drawCircle(ringCentreX, ringCentreY, ringRadius, colour);
  if (ringRadius >= 7) {
    // A one-pixel ring reads as a hairline next to a 24pt *bold* numeral,
    // which is the one place it must not look like an artefact. A second
    // concentric circle gives it a stroke weight in the same family as the
    // digits it is standing beside. Skipped at small radii, where a 2px
    // stroke would close the ring into a dot.
    lcd.drawCircle(ringCentreX, ringCentreY, ringRadius - 1, colour);
  }
  cursorX += ringRadius * 2 + gap;

  lcd.drawString(unit, cursorX, y);
}

// ---------------------------------------------------------------------------
// Condition icons for the forecast card (see showForecastCard() below and
// the README's "The weather card retired, folded into Forecast"). Hand-drawn
// with LovyanGFX primitives rather than a bitmap asset - the same reasoning
// drawTemperature()'s own hand-drawn degree ring applies to this font's
// missing glyph: nothing here can fail to decode, needs an SD card, or costs
// a network fetch. `radius` is a parameter, not a constant, so the identical
// drawing code serves both the forecast card's larger hero icon and its five
// small strip icons - see showForecastCard()'s two call sites below.
// ---------------------------------------------------------------------------

enum class WeatherIconKind {
  Sunny,
  PartlyCloudy,
  Cloudy,
  Rain,
  Storm,
  Snow,
  Fog,
  Unknown,
};

// Amber, not a theme colour - same reasoning kWarn stays outside the
// day/night swap: a sun glyph needs to read as "sun-coloured" against both a
// white day background and a black night one, and this happens to share
// kWarn's own value rather than inventing a second amber.
constexpr uint32_t kIconSun = 0xEDA100u;
// A medium blue, chosen for the same reason: legible on white and on black,
// unlike either a pale blue (vanishes on white) or a dark navy (vanishes on
// black).
constexpr uint32_t kIconRain = 0x3F7FD9u;
constexpr uint32_t kIconSnow = 0x7FB8E0u;

// NWS's shortForecast is free text ("Chance Showers And Thunderstorms then
// Partly Sunny"), not a coded enum - there is no field to switch on, only a
// phrase to guess from. Checked in an order that puts the more specific,
// more visually distinct conditions first (a "Thunderstorm" is also technically
// a "Rain" event, but showing the bolt is the more useful read), and falls
// through to Unknown - a plain cloud outline - for anything unrecognised
// rather than guessing wrong. Case-insensitive since NWS capitalises whole
// words ("Showers") inconsistently with how a household might describe the
// same day.
WeatherIconKind classifyCondition(const String& shortForecast) {
  String lower = shortForecast;
  lower.toLowerCase();
  if (lower.indexOf("thunder") >= 0 || lower.indexOf("storm") >= 0) {
    return WeatherIconKind::Storm;
  }
  if (lower.indexOf("snow") >= 0 || lower.indexOf("flurr") >= 0 || lower.indexOf("sleet") >= 0) {
    return WeatherIconKind::Snow;
  }
  if (lower.indexOf("rain") >= 0 || lower.indexOf("shower") >= 0 || lower.indexOf("drizzle") >= 0) {
    return WeatherIconKind::Rain;
  }
  if (lower.indexOf("fog") >= 0 || lower.indexOf("haze") >= 0 || lower.indexOf("mist") >= 0) {
    return WeatherIconKind::Fog;
  }
  if (lower.indexOf("partly") >= 0 || lower.indexOf("mostly cloudy") >= 0 ||
      lower.indexOf("mostly sunny") >= 0 || lower.indexOf("mostly clear") >= 0) {
    return WeatherIconKind::PartlyCloudy;
  }
  if (lower.indexOf("cloud") >= 0 || lower.indexOf("overcast") >= 0) {
    return WeatherIconKind::Cloudy;
  }
  if (lower.indexOf("clear") >= 0 || lower.indexOf("sunny") >= 0 || lower.indexOf("fair") >= 0) {
    return WeatherIconKind::Sunny;
  }
  return WeatherIconKind::Unknown;
}

// Three overlapping filled circles plus a base rect, scaled off `radius` -
// the smallest shape that still reads as a cloud silhouette rather than
// three separate dots at the sizes this card actually draws it (14px radius
// in the strip, 26px in the hero).
void drawCloudShape(int cx, int cy, int radius, uint32_t colour) {
  lcd.fillCircle(cx - radius * 0.35f, cy, radius * 0.42f, colour);
  lcd.fillCircle(cx + radius * 0.05f, cy - radius * 0.18f, radius * 0.5f, colour);
  lcd.fillCircle(cx + radius * 0.5f, cy + radius * 0.05f, radius * 0.35f, colour);
  lcd.fillRect(cx - radius * 0.35f, cy, radius * 0.9f, radius * 0.4f, colour);
}

// Filled disc plus eight rays, shared by the forecast card's Sunny icon and
// the sunrise row of showSunMoonCard() below - one drawing routine rather
// than two copies that could drift apart. Eight ray directions as
// precomputed unit vectors (cos/sin of 0/45/90.../315 degrees) rather than
// calling cosf/sinf at draw time - same "do not lean on a platform feature
// that might not be there" reasoning as this file's own compassDirection()
// 8-point table and drawTemperature()'s hand-drawn degree ring, just applied
// to trig instead of locale/glyph support.
void drawSunIcon(int cx, int cy, int radius) {
  lcd.fillCircle(cx, cy, radius * 0.5f, kIconSun);
  static constexpr float kRayDirs[8][2] = {
      {1.0f, 0.0f},   {0.71f, 0.71f},  {0.0f, 1.0f},   {-0.71f, 0.71f},
      {-1.0f, 0.0f},  {-0.71f, -0.71f}, {0.0f, -1.0f},  {0.71f, -0.71f},
  };
  for (const auto& dir : kRayDirs) {
    const int x0 = cx + static_cast<int>(dir[0] * radius * 0.65f);
    const int y0 = cy + static_cast<int>(dir[1] * radius * 0.65f);
    const int x1 = cx + static_cast<int>(dir[0] * radius * 0.95f);
    const int y1 = cy + static_cast<int>(dir[1] * radius * 0.95f);
    lcd.drawLine(x0, y0, x1, y1, kIconSun);
  }
}

// A crescent, for showSunMoonCard()'s sunset row - the same "second circle
// carves a bite out of the first" technique showMoonPhaseCard()'s own
// terminator-ellipse trick uses, simplified to a fixed crescent rather than
// a phase-accurate disc: this icon means "it is night now", not "tonight's
// specific moon phase" (that distinction belongs to the moonphase card
// alone). The bite is cut with bg() rather than muted(), so it reads as a
// true gap down to the card background in both themes rather than a filled
// grey shadow.
void drawMoonIcon(int cx, int cy, int radius) {
  lcd.fillCircle(cx, cy, radius * 0.5f, ink());
  lcd.fillCircle(cx + radius * 0.22f, cy - radius * 0.15f, radius * 0.42f, bg());
}

// A wave (two stacked shallow arcs, drawn as short line segments rather than
// a true arc call - see the sun icon's own remarks on not leaning on a
// platform feature this file does not already use elsewhere) with an arrow
// above it, for showTidesCard()'s two rows. `rising` picks the arrow
// direction - up for the next high tide, down for the next low - the same
// distinction a household actually cares about ("is the water coming in or
// going out"), which neither row's own label states outright.
void drawTideIcon(int cx, int cy, int radius, bool rising) {
  // Each wave is 4 points (5 segments would overrun waveWidth) alternating
  // above/below waveY, connected point to point - a plain zigzag rather than
  // a true sine curve, same "shape reads as a wave at icon size" standard
  // the cloud/lightning-bolt icons above already accept.
  const int waveWidth = radius * 1.3f;
  const int x0 = cx - waveWidth / 2;
  for (int row = 0; row < 2; ++row) {
    const int waveY = cy + radius * 0.1f + row * radius * 0.45f;
    int prevX = x0;
    int prevY = waveY;
    for (int point = 1; point <= 4; ++point) {
      const int x = x0 + point * waveWidth / 4;
      const int y = waveY + ((point % 2 == 0) ? -radius * 0.15f : radius * 0.15f);
      lcd.drawLine(prevX, prevY, x, y, kIconRain);
      prevX = x;
      prevY = y;
    }
  }

  const int arrowBaseY = rising ? cy - radius * 0.75f : cy - radius * 0.25f;
  const int arrowTipY = rising ? arrowBaseY - radius * 0.4f : arrowBaseY + radius * 0.4f;
  lcd.fillTriangle(cx - radius * 0.22f, arrowBaseY, cx + radius * 0.22f, arrowBaseY, cx, arrowTipY,
                   ink());
}

void drawWeatherIcon(WeatherIconKind kind, int cx, int cy, int radius) {
  switch (kind) {
    case WeatherIconKind::Sunny:
      drawSunIcon(cx, cy, radius);
      break;
    case WeatherIconKind::PartlyCloudy:
      lcd.fillCircle(cx - radius * 0.3f, cy - radius * 0.3f, radius * 0.38f, kIconSun);
      drawCloudShape(cx + radius * 0.15f, cy + radius * 0.15f, radius * 0.85f, muted());
      break;
    case WeatherIconKind::Cloudy:
      drawCloudShape(cx, cy, radius, muted());
      break;
    case WeatherIconKind::Rain:
      drawCloudShape(cx, cy - radius * 0.15f, radius * 0.85f, muted());
      for (int i = -1; i <= 1; i++) {
        const int x0 = cx + i * radius * 0.35f;
        lcd.drawLine(x0, cy + radius * 0.4f, x0 - radius * 0.15f, cy + radius * 0.8f, kIconRain);
      }
      break;
    case WeatherIconKind::Storm: {
      drawCloudShape(cx, cy - radius * 0.15f, radius * 0.85f, muted());
      // A simple zigzag bolt, three points wide - fillTriangle rather than a
      // stroked polyline so it reads as a solid bolt, not a thin scratch, at
      // the small sizes this icon draws at.
      const int bx = cx, by = cy + radius * 0.35f;
      lcd.fillTriangle(bx, by, bx + radius * 0.3f, by, bx - radius * 0.05f,
                       by + radius * 0.35f, kIconSun);
      lcd.fillTriangle(bx - radius * 0.05f, by + radius * 0.35f, bx + radius * 0.25f,
                       by + radius * 0.35f, bx - radius * 0.2f, by + radius * 0.75f, kIconSun);
      break;
    }
    case WeatherIconKind::Snow:
      drawCloudShape(cx, cy - radius * 0.15f, radius * 0.85f, muted());
      for (int i = -1; i <= 1; i++) {
        lcd.fillCircle(cx + i * radius * 0.35f, cy + radius * 0.6f, radius * 0.09f, kIconSnow);
      }
      break;
    case WeatherIconKind::Fog:
      for (int i = 0; i < 4; i++) {
        const int y = cy - radius * 0.3f + i * radius * 0.22f;
        lcd.drawFastHLine(cx - radius * 0.7f, y, radius * 1.4f, muted());
      }
      break;
    case WeatherIconKind::Unknown:
    default:
      // No confident match - an outline-only cloud rather than guessing at a
      // specific condition the phrase never actually named.
      drawCloudShape(cx, cy, radius, muted());
      break;
  }
}

// ADS-B's "track" field is degrees clockwise from true north (0=N, 90=E,
// ...) - same 8-point compass lookup as CYD-Dickey's compassDirection(),
// used identically here to turn Aircraft::Sighting::headingDegrees into
// something readable without printing a raw degree number (which would
// need its own degree-glyph workaround for no real benefit - CYD-Dickey's
// aircraft card doesn't show the raw number either, only the compass
// letter).
const char* compassDirection(double degrees) {
  static const char* dirs[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  int index = (static_cast<int>((degrees + 22.5) / 45.0)) % 8;
  if (index < 0) index += 8;
  return dirs[index];
}

// Comma-grouped whole-dollar price ("$450,000") - manual grouping since
// ESP32 libc locale support is not to be relied on, the same "do not trust a
// platform feature that might not be there" reasoning this file's hand-drawn
// degree ring already applies to a font's missing degree glyph.
String formatPrice(int price) {
  const String digits = String(price);
  String grouped;
  int sinceComma = 0;
  for (int i = digits.length() - 1; i >= 0; --i) {
    grouped = digits[i] + grouped;
    sinceComma++;
    if (sinceComma % 3 == 0 && i > 0) {
      grouped = "," + grouped;
    }
  }
  return "$" + grouped;
}

// Drops a whole number's trailing ".0" ("3" not "3.0") and keeps one decimal
// otherwise ("3.5") - RentCast reports bedrooms/bathrooms as fractional (a
// half-bath is a real, common listing detail), and printing every value at a
// fixed one decimal place would render "3.0 bd" for the ordinary whole-number
// case, which reads as a fetch glitch rather than a deliberate number to
// someone glancing at this from across a room.
String formatCount(double value) {
  const int whole = static_cast<int>(value + 0.5);
  double diff = value - whole;
  if (diff < 0) diff = -diff;
  if (diff < 0.05) {
    return String(whole);
  }
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%.1f", value);
  return String(buffer);
}

// "New today"/"1 day"/"N days" - same singular/plural care
// describeFreshness() already gives "Updated 1 min ago" elsewhere in this
// file, applied to RentCast's daysOnMarket instead of a fetch age.
String formatDaysOnMarket(int days) {
  if (days <= 0) {
    return "New today";
  }
  if (days == 1) {
    return "1 day";
  }
  return String(days) + " days";
}

}  // namespace

void begin() {
  lcd.init();
  lcd.setRotation(1);  // 320x240 landscape
  lcd.setBrightness(255);
  clear();
}

void setEnvironment(int utcOffsetMinutes, bool isDaytime) {
  gUtcOffsetMinutes = utcOffsetMinutes;
  gIsDaytime = isDaytime;
}

int utcOffsetMinutes() { return gUtcOffsetMinutes; }

bool readTouchRaw(int32_t& x, int32_t& y) {
  return lcd.getTouch(&x, &y);
}

void showStatus(const String& headline, const String& detail) {
  clear();
  const int headlineLines = wrappedCenteredText(headline, 85, ink(), 2, 22, 3);
  if (detail.length() > 0) {
    wrappedCenteredText(detail, 85 + headlineLines * 22 + 12, muted(), 1, 14, 3);
  }
}

void showFailure(const String& headline, const String& whatToDo) {
  clear();
  const int headlineLines = wrappedCenteredText(headline, 75, kWarn, 2, 22, 3);
  wrappedCenteredText(whatToDo, 75 + headlineLines * 22 + 12, muted(), 1, 14, 3);
}

void aircraftLogoZone(int16_t& x, int16_t& y, int16_t& w, int16_t& h) {
  // Top-right of the content area: clear of the banner (ends y22), clear of
  // the headline's left-aligned start (truncated to stop at x210, see
  // below), and above the distance/route line at y64 so a wide logo cannot
  // run into it either.
  x = 220;
  y = 26;
  w = 90;
  h = 34;
}

void showAircraftCard(const String& callsign, const String& airlineName, int altitudeFeet,
                      double speedKnots, double headingDegrees, double distanceMiles,
                      const String& originCode, const String& destinationCode,
                      const String& updatedAt) {
  lcd.fillScreen(bg());
  drawCardBanner("OVERHEAD", kAircraftBanner, 130);

  // Airline name is the headline when the server has one, in the spot
  // CYD-Dickey's drawFeaturedAircraft() gives the airline logo or bold
  // airline name - callsign was the fallback for this position for as long
  // as this server sent nothing richer (see Aircraft.h's updated remarks),
  // and stays the fallback now for a server too old to send a name at all.
  // Truncated left at 200px, not the full card width: aircraftLogoZone()
  // starts at x220, and a name long enough to reach it would run under the
  // logo rather than stopping short of it.
  const bool hasAirlineName = airlineName.length() > 0;
  const String headline = hasAirlineName ? airlineName : callsign;
  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextSize(1);
  lcd.setTextColor(ink(), bg());
  drawTruncatedLeft(headline, kCardMargin, 32, 200);

  // Callsign drops to this secondary line, alongside distance, only when the
  // airline name took the headline slot above it - otherwise callsign is
  // already the headline and repeating it here would be the same fact twice.
  lcd.setFont(&fonts::FreeSansBold9pt7b);
  // Plain ASCII separator, not a middle-dot or any other non-ASCII glyph -
  // this font has no Unicode coverage (see drawTemperature's hand-drawn
  // degree ring for the same constraint hit and worked around elsewhere in
  // this file).
  char distanceBuf[32];
  if (hasAirlineName) {
    snprintf(distanceBuf, sizeof(distanceBuf), "%s - %.1f mi away", callsign.c_str(), distanceMiles);
  } else {
    snprintf(distanceBuf, sizeof(distanceBuf), "%.1f mi away", distanceMiles);
  }
  lcd.setTextColor(muted(), bg());
  lcd.drawString(distanceBuf, kCardMargin, 64);

  // Route, in the gap between the distance line and the stat rows. Codes
  // only, not names: two airport names plus everything else on this card
  // does not fit readably on a 320x240 panel (see Display.h's own remarks).
  // Neither code present draws no line at all - the honest rendering of "no
  // route data", the same reasoning Graphic.cpp draws nothing rather than an
  // empty frame when it has no picture configured.
  if (originCode.length() > 0) {
    lcd.setFont(&fonts::FreeSansBold9pt7b);
    lcd.setTextColor(muted(), bg());
    // "->" rather than a real arrow glyph, for the same reason as the
    // separator above - plain ASCII only.
    const String routeLine = destinationCode.length() > 0
        ? (originCode + " -> " + destinationCode)
        : ("from " + originCode);
    lcd.drawString(routeLine, kCardMargin, 82);
  }

  // Stat rows: a muted label on the left, the value right-justified against
  // the card's right margin - the same truncate-and-right-justify technique
  // as CYD-Dickey's drawFeaturedAircraft()/drawTruncatedRight (see
  // drawRightJustified above), applied per-row here instead of to a whole
  // second column of airline-specific fields CAL didn't used to have data
  // for.
  const int rightX = kScreenW - kCardMargin;
  const int rowValueWidth = 150;
  int rowY = 100;
  constexpr int kRowHeight = 30;

  lcd.setTextColor(muted(), bg());
  lcd.drawString("Altitude", kCardMargin, rowY);
  lcd.setTextColor(ink(), bg());
  drawRightJustified(String(altitudeFeet) + " ft", rightX, rowY, rowValueWidth);
  rowY += kRowHeight;

  lcd.setTextColor(muted(), bg());
  lcd.drawString("Speed", kCardMargin, rowY);
  lcd.setTextColor(ink(), bg());
  drawRightJustified(String(static_cast<int>(speedKnots + 0.5)) + " kts", rightX, rowY, rowValueWidth);
  rowY += kRowHeight;

  lcd.setTextColor(muted(), bg());
  lcd.drawString("Heading", kCardMargin, rowY);
  lcd.setTextColor(ink(), bg());
  drawRightJustified(compassDirection(headingDegrees), rightX, rowY, rowValueWidth);
  rowY += kRowHeight;

  // Same defect Weather.cpp's restyle found and fixed on its own card: this
  // line was set in Font0, the 6x8 bitmap face drawClock()'s own remarks
  // record as having failed on real hardware ("simply not there"). Same
  // fix, same face.
  if (updatedAt.length() > 0) {
    lcd.setFont(&fonts::FreeSansBold9pt7b);
    lcd.setTextSize(1);
    lcd.setTextColor(muted(), bg());
    lcd.setTextDatum(top_left);
    lcd.drawString(updatedAt, kCardMargin, rowY + 4);
  }

  drawClock();
  restoreDefaultFont();
}

void showAircraftStatus(const String& headline, const String& detail, bool isProblem) {
  lcd.fillScreen(bg());
  drawCardBanner("OVERHEAD", kAircraftBanner, 130);

  lcd.setFont(&fonts::FreeSansBold9pt7b);
  lcd.setTextSize(1);
  const uint32_t headlineColour = isProblem ? kWarn : muted();
  const int headlineLines =
      wrappedLeftText(headline, kCardMargin, 40, headlineColour, 22, 3, kScreenW - kCardMargin * 2);
  if (detail.length() > 0) {
    wrappedLeftText(detail, kCardMargin, 40 + headlineLines * 22 + 12, ink(), 18, 3,
                    kScreenW - kCardMargin * 2);
  }

  drawClock();
  restoreDefaultFont();
}

void showSunMoonCard(const String& sunriseText, const String& sunsetText, const String& detail) {
  lcd.fillScreen(bg());
  drawCardBanner("SUN", kSunMoonBanner, 70);

  // Two rows, label left and time right-justified, reusing showAircraftCard's
  // stat-row layout rather than inventing a second one - this card is the same
  // shape of information (a short label against a short value).
  const int rightX = kScreenW - kCardMargin;
  const int rowValueWidth = 150;

  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextSize(1);

  lcd.setTextColor(muted(), bg());
  lcd.setTextDatum(top_left);
  lcd.drawString("Sunrise", kCardMargin, 44);
  lcd.setTextColor(ink(), bg());
  drawRightJustified(sunriseText, rightX, 44, rowValueWidth);

  lcd.setTextColor(muted(), bg());
  lcd.drawString("Sunset", kCardMargin, 90);
  lcd.setTextColor(ink(), bg());
  drawRightJustified(sunsetText, rightX, 90, rowValueWidth);

  // One icon per row, in the gap between the label and the right-justified
  // value column - "Sunrise"/"Sunset" at this font leave roughly x100-160
  // empty, plenty of room for a 24px icon without touching either column.
  // Sun for sunrise, a crescent for sunset - the card's own name ("sun and
  // moon") made this the obvious pairing rather than drawing a sun on both
  // rows and leaving "moon" in the id unrepresented anywhere on the card.
  // Radius 16 (32px across) rather than the original 12 - reported too small to
  // read at a glance on real hardware. Still clears both columns either side:
  // "Sunrise"/"Sunset" at this font end around x=90, the value column starts at
  // x=160, and a 32px icon centred at x=130 spans 114-146.
  constexpr int kIconColumnX = 130;
  constexpr int kIconRadius = 16;
  drawSunIcon(kIconColumnX, 44 + 9, kIconRadius);
  drawMoonIcon(kIconColumnX, 90 + 9, kIconRadius);

  if (detail.length() > 0) {
    lcd.setFont(&fonts::FreeSansBold9pt7b);
    lcd.setTextColor(muted(), bg());
    wrappedLeftText(detail, kCardMargin, 140, muted(), 20, 2, kScreenW - kCardMargin * 2);
  }

  drawClock();
  restoreDefaultFont();
}

void showTidesCard(const String& nextHighTideText, const String& nextLowTideText) {
  lcd.fillScreen(bg());
  drawCardBanner("TIDES", kTidesBanner, 90);

  // Same stat-row layout as showSunMoonCard() immediately above: two rows,
  // label left and time right-justified. No third line here - see Tides.h
  // and this function's own declaration in Display.h for why a tide has no
  // "detail" worth adding.
  const int rightX = kScreenW - kCardMargin;
  const int rowValueWidth = 150;

  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextSize(1);

  lcd.setTextColor(muted(), bg());
  lcd.setTextDatum(top_left);
  lcd.drawString("Next high", kCardMargin, 44);
  lcd.setTextColor(ink(), bg());
  drawRightJustified(nextHighTideText, rightX, 44, rowValueWidth);

  lcd.setTextColor(muted(), bg());
  lcd.drawString("Next low", kCardMargin, 90);
  lcd.setTextColor(ink(), bg());
  drawRightJustified(nextLowTideText, rightX, 90, rowValueWidth);

  // Same icon-in-the-gap placement as showSunMoonCard()'s two rows -
  // "Next high"/"Next low" are wider labels than "Sunrise"/"Sunset" at this
  // font, so the column sits a little further right to stay clear of them.
  // Same size bump as showSunMoonCard()'s icons, for the same reported reason -
  // "Next high"/"Next low" leave a narrower gap (ending ~x110 vs. the value
  // column's x160), so radius 16 centred at x=135 (119-151) rather than
  // showSunMoonCard()'s x=130, to keep clearance on both sides.
  constexpr int kIconColumnX = 135;
  constexpr int kIconRadius = 16;
  drawTideIcon(kIconColumnX, 44 + 9, kIconRadius, /*rising=*/true);
  drawTideIcon(kIconColumnX, 90 + 9, kIconRadius, /*rising=*/false);

  drawClock();
  restoreDefaultFont();
}

void showIssFlyoverCard(const String& distanceText, const String& directionText,
                        const String& detail) {
  lcd.fillScreen(bg());
  drawCardBanner("ISS", kIssFlyoverBanner, 90);

  // Same stat-row layout as showSunMoonCard()/showTidesCard(): two rows,
  // label left and value right-justified, plus the same third `detail` line
  // showSunMoonCard() uses for day length - here, the actual coordinates.
  const int rightX = kScreenW - kCardMargin;
  const int rowValueWidth = 150;

  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextSize(1);

  lcd.setTextColor(muted(), bg());
  lcd.setTextDatum(top_left);
  lcd.drawString("Distance", kCardMargin, 44);
  lcd.setTextColor(ink(), bg());
  drawRightJustified(distanceText, rightX, 44, rowValueWidth);

  lcd.setTextColor(muted(), bg());
  lcd.drawString("Direction", kCardMargin, 90);
  lcd.setTextColor(ink(), bg());
  drawRightJustified(directionText, rightX, 90, rowValueWidth);

  if (detail.length() > 0) {
    lcd.setFont(&fonts::FreeSansBold9pt7b);
    lcd.setTextColor(muted(), bg());
    wrappedLeftText(detail, kCardMargin, 140, muted(), 20, 2, kScreenW - kCardMargin * 2);
  }

  drawClock();
  restoreDefaultFont();
}

// The Moon-phase card - the first "graphical style" card: an actual drawn
// disc rather than a text description, captioned with phaseName underneath.
//
// **Rendering technique - "half-disc plus terminator ellipse"**, a well-known
// way to fake a lunar-phase disc with nothing but circle/ellipse primitives:
//
//   1. Fill the whole disc muted() - a starting assumption that none of it is
//      lit.
//   2. Fill exactly the half of it currently facing the Sun in ink(), as a
//      half-disc wedge (fillArc from radius 0 to radius, sweeping 180
//      degrees) rather than a fillRect: a rectangle's bounding box has
//      corners outside the circle that a wedge does not, which would
//      otherwise poke square corners past the round limb into the card
//      background.
//   3. Overlay an ellipse - same centre, same vertical radius as the disc,
//      horizontal radius `disc radius * |1 - 2 * illuminatedFraction|` - to
//      grow or shrink the lit area away from the exact-half case step 2
//      drew:
//        - illuminatedFraction < 0.5 (crescent): the ellipse is filled
//          muted(), eating back into the lit half. At illuminatedFraction 0
//          the ellipse's horizontal radius equals the disc's own, so it
//          coincides with the outer circle and the whole disc reads dark -
//          new moon.
//        - illuminatedFraction > 0.5 (gibbous): the ellipse is filled ink(),
//          growing into the still-dark half. At illuminatedFraction 1 it
//          likewise coincides with the outer circle and the whole disc
//          reads lit - full moon.
//        - At exactly 0.5 the ellipse has zero width, so it is skipped
//          rather than drawn as a no-op; step 2's half-disc is already the
//          right answer (first or last quarter).
//
// **Waxing/waning convention.** `phase` < 0.5 is waxing (growing toward
// full) and lights the right half in step 2; `phase` > 0.5 is waning
// (shrinking toward new) and lights the left half. This is the Northern
// Hemisphere convention - a waxing crescent's illuminated limb is on the
// right as seen looking up from the northern half of the planet. A Southern
// Hemisphere household sees its own sky mirrored left-right from what this
// draws. That is a deliberate, documented simplification (see the README),
// not an oversight: there is no per-device hemisphere signal today to draw
// the correct picture from, and the alternative - drawing neither
// convention correctly for anyone - is worse than picking one and saying
// so.
//
// UNVERIFIED ON HARDWARE, same as every other card in this file - checked
// by a clean compile and by reading, not by a real decode on a real panel.
void showMoonPhaseCard(const String& phaseName, double phase, double illuminatedFraction) {
  lcd.fillScreen(bg());
  drawCardBanner("MOON", kMoonPhaseBanner, 80);

  const int cx = kScreenW / 2;
  const int cy = 90;
  const int radius = 50;

  // Defensive clamp only - MoonPhase.cpp's cardItemCount() already keeps this
  // function from being called at all with the "no data" sentinel (-1), so
  // this never actually sees an out-of-range value in practice.
  double k = illuminatedFraction;
  if (k < 0.0) k = 0.0;
  if (k > 1.0) k = 1.0;
  const bool waxingRight = phase < 0.5;

  lcd.fillCircle(cx, cy, radius, muted());
  if (waxingRight) {
    lcd.fillArc(cx, cy, 0, radius, 270, 90, ink());
  } else {
    lcd.fillArc(cx, cy, 0, radius, 90, 270, ink());
  }

  double halfWidthFraction = 2.0 * k - 1.0;
  if (halfWidthFraction < 0.0) halfWidthFraction = -halfWidthFraction;
  const int terminatorRx = static_cast<int>(radius * halfWidthFraction + 0.5);
  if (terminatorRx > 0) {
    const uint32_t terminatorColour = (k <= 0.5) ? muted() : ink();
    lcd.fillEllipse(cx, cy, terminatorRx, radius, terminatorColour);
  }

  // A crisp outline regardless of theme: muted() against bg() is legible
  // elsewhere in this file as body text, but a ring makes the disc's edge
  // unambiguous even where the two are close in tone.
  lcd.drawCircle(cx, cy, radius, ink());

  if (phaseName.length() > 0) {
    lcd.setFont(&fonts::FreeSansBold12pt7b);
    wrappedCenteredText(phaseName, 150, ink(), 1, 22, 1);
  }

  char pctBuffer[24];
  snprintf(pctBuffer, sizeof(pctBuffer), "%d%% illuminated", static_cast<int>(k * 100.0 + 0.5));
  lcd.setFont(&fonts::FreeSansBold9pt7b);
  wrappedCenteredText(String(pctBuffer), 176, muted(), 1, 18, 1);

  drawClock();
  restoreDefaultFont();
}

// The hero number and only the hero number - see Display.h's own remarks on
// why there is no banner here. Sized by doubling FreeSansBold24pt7b with
// setTextSize(2) rather than reaching for a bigger font file: the weather
// card already proved this exact face legible on this panel at size 1, and
// LovyanGFX's setTextSize scales a GFX font's rendered glyphs cleanly, so
// this gets a genuinely room-filling clock face without adding a second
// 24pt-class font to the binary for a five-character string.
//
// Width is measured rather than assumed before committing to size 2: "HH:MM"
// is short, but this file has already been burned once by an assumption
// about a font's real on-panel size turning out wrong (see drawClock()'s own
// remarks on the corner clock's original, invisible 6x8 bitmap attempt). A
// clock that would run off both edges of a 320px panel falls back to size 1
// instead - still exactly the weather hero's own proven size - rather than
// clipping.
void showClockDate(const String& timeText, const String& dateText) {
  lcd.fillScreen(bg());

  lcd.setFont(&fonts::FreeSansBold24pt7b);
  lcd.setTextColor(ink(), bg());
  lcd.setTextDatum(middle_center);

  lcd.setTextSize(2);
  const int maxTimeWidth = kScreenW - kCardMargin * 2;
  if (lcd.textWidth(timeText) > maxTimeWidth) {
    lcd.setTextSize(1);
  }
  lcd.drawString(timeText, kScreenW / 2, 100);

  // The date, secondary to the time both in size and in colour (muted(),
  // same as every other card's supporting line) - the same bold 9pt/12pt
  // family the rest of this file uses rather than a plain bitmap face, and
  // reusing wrappedCenteredText's own word-wrap/measure logic (see
  // showStatus() above) rather than assuming a spelled-out weekday and month
  // always fits on one line at this width.
  lcd.setFont(&fonts::FreeSansBold12pt7b);
  wrappedCenteredText(dateText, 145, muted(), 1, 20, 2);

  drawClock();
  restoreDefaultFont();
}

// The announcement card: an admin's free text, filling most of the panel.
// Styled after showWeatherCard()'s own two-tier sizing for its shortForecast
// phrase (see that function's remarks) rather than a fixed size, for the same
// reason - this text comes from a server with no length this file controls
// beyond CardPolicyEditing.MaxTextLength (280 characters, enforced there, not
// here), and truncating an admin's sentence can change what it says ("no
// school tomorrow" clipped to "no school" is a materially different notice).
// The larger size is tried first and used whenever the whole text actually
// fits in it; only text that would overflow it drops to the smaller, denser
// size, so a short reminder is never shown smaller than it needs to be.
void showAnnouncementCard(const String& text) {
  lcd.fillScreen(bg());
  drawCardBanner("NOTICE", kAnnouncementBanner, 90);

  const int bodyWidth = kScreenW - kCardMargin * 2;
  if (text.length() > 0) {
    lcd.setFont(&fonts::FreeSansBold12pt7b);
    lcd.setTextSize(1);
    // Starts at y=30 rather than the original y=40 - kButtonRowY moved from
    // 190 to 160 when the button row doubled in height, and shifting this
    // block's own start up by 10px buys back exactly the clearance that
    // move cost. 5 lines at 24px is now y 30-150, clear of the button row by
    // 10px.
    const int linesAtLargeSize =
        wrappedLeftText(text, kCardMargin, 30, ink(), 24, 5, bodyWidth, /*measureOnly=*/true);
    if (linesAtLargeSize <= 5) {
      wrappedLeftText(text, kCardMargin, 30, ink(), 24, 5, bodyWidth);
    } else {
      // 7 lines at 18px is y 30-156, a tighter but still real 4px clearance
      // at the smaller size - and 7 lines of roughly 38 characters each
      // still comfortably covers the full 280-character limit without a
      // further fallback tier.
      lcd.setFont(&fonts::FreeSansBold9pt7b);
      wrappedLeftText(text, kCardMargin, 30, ink(), 18, 7, bodyWidth);
    }
  }

  drawClock();
  restoreDefaultFont();
}

// The QR card: a scannable code with an optional caption, ported from CAL's own
// bootloader-side showQr() (root Display.cpp) - same vendored CalQr.h/.c, same fixed
// version-6/ECC-LOW static buffer sizing, same qrcode_initText()/qrcode_getModule() render
// loop. This is proven, working code (CAL's own setup-wizard "scan this to configure WiFi"
// screen), so the rendering technique itself is carried over verbatim; only the surrounding
// layout changes, because this card has to share the panel with a banner above it, a
// caption/fallback line beneath it, and this file's own button row and corner clock further
// down - CAL's own showQr() owns the whole 240px-tall panel and answers to nothing else.
//
// scale is 2 here (a 90px code) rather than CAL's own 3 (135px, on a screen with nothing
// else on it): kButtonRowY starts at 160 (moved up from 190 when the button row doubled in
// height - see that constant's own remarks) and the banner already claims the top 22px, so
// this card's whole drawable height is roughly 130px against CAL's ~228, and there is a
// caption line and a fallback data line still to fit beneath the code. A smaller code is the
// honest trade-off for sharing a smaller card - see this file's own UNVERIFIED-ON-HARDWARE
// caveat in QrText.h, since scan reliability at this size has not been checked against a
// real camera on real glass.
//
// The button row's move cost this card real margin it did not have much of to spare, so the
// fallback qrData line beneath the code is capped at one line, not two, whenever a caption is
// also present - the caption already gives the code a label, so the raw payload text becomes
// belt-and-suspenders rather than the only one, and one line is what the remaining budget
// below actually clears. Only when there is no caption - qrData is then the only label this
// card has - does it still get the full two lines a maximum-length payload could need.
//
// caption is optional and drawn above the raw payload, exactly the role CAL's own
// caption/subCaption parameters play beneath its code - supplementary, not the point. The
// raw qrData is always drawn beneath the code regardless of whether caption is present,
// mirroring CAL's own remark on its equivalent line: "the address in characters as well as
// in the code, because cameras fail."
void showQrTextCard(const String& qrData, const String& caption) {
  lcd.fillScreen(bg());
  drawCardBanner("SCAN", kQrTextBanner, 90);

  const int bodyWidth = kScreenW - kCardMargin * 2;

  // Same fixed QR parameters as CAL's own bootloader showQr(): version 6 at ECC LOW, a
  // ~134-byte byte-mode capacity comfortably above CardPolicyEditing.MaxQrDataLength (100) -
  // see that server-side constant's own remarks for the derivation. The buffer is sized
  // here rather than by qrcode_getBufferSize(), which is a runtime function in this library
  // and so cannot size a static array - same arithmetic, evaluated at compile time, as the
  // reference implementation.
  static constexpr uint8_t kQrVersion = 6;
  static constexpr size_t kQrModules = kQrVersion * 4 + 17;                    // 41
  static constexpr size_t kQrBufferBytes = (kQrModules * kQrModules + 7) / 8;  // 211

  QRCode qr;
  static uint8_t qrBuffer[kQrBufferBytes];
  if (qrData.length() == 0 ||
      qrcode_initText(&qr, qrBuffer, kQrVersion, ECC_LOW, qrData.c_str()) != 0) {
    // qrData.length() == 0 is only reachable if the policy changed between the
    // scheduler's itemCount() check and this call - see Announcement::cardDraw()'s
    // identical remark on its own equivalent guard. A non-zero qrcode_initText()
    // return is data too long for this fixed version to encode - defensive only,
    // since CardPolicyEditing.MaxQrDataLength already keeps an ordinary saved
    // policy well clear of that limit - handled by a failure message rather than
    // drawing garbage, the same choice CAL's own showQr() makes.
    wrappedLeftText("Cannot display code", kCardMargin, 60, ink(), 22, 2, bodyWidth);
    if (qrData.length() > 0) {
      wrappedLeftText(qrData, kCardMargin, 110, muted(), 18, 3, bodyWidth);
    }
    drawClock();
    restoreDefaultFont();
    return;
  }

  const int quiet = 2;
  const int modules = qr.size + quiet * 2;
  const int scale = 2;
  const int side = modules * scale;
  const int x0 = (kScreenW - side) / 2;
  const int y0 = 28;

  lcd.fillRect(x0, y0, side, side, 0xFFFFFFu);
  for (uint8_t y = 0; y < qr.size; ++y) {
    for (uint8_t x = 0; x < qr.size; ++x) {
      if (qrcode_getModule(&qr, x, y)) {
        lcd.fillRect(x0 + (x + quiet) * scale, y0 + (y + quiet) * scale, scale, scale, 0x000000u);
      }
    }
  }

  int y = y0 + side + 6;
  const bool hasCaption = caption.length() > 0;
  if (hasCaption) {
    lcd.setFont(&fonts::FreeSansBold12pt7b);
    y += wrappedCenteredText(caption, y, ink(), 1, 22, 1) * 22;
    y += 2;
  }
  lcd.setFont(&fonts::FreeSansBold9pt7b);
  // One line, not two, once a caption is already on screen - see this
  // function's own header comment for why.
  wrappedCenteredText(qrData, y, muted(), 1, 16, hasCaption ? 1 : 2);

  drawClock();
  restoreDefaultFont();
}

void showListingsCard(const String& address, const String& propertyType, int price,
                      double bedrooms, double bathrooms, int squareFootage,
                      int daysOnMarket, double distanceMiles, uint16_t index,
                      uint16_t total, const String& updatedAt) {
  lcd.fillScreen(bg());
  drawCardBanner("LISTINGS", kListingsBanner, 130);

  // "2 of 5" - drawn in the banner row but outside the coloured rect (which
  // ends at x=130), so it costs no space the address/price block below needs
  // and reads as ordinary chrome rather than competing with the card's own
  // content. Only drawn once there is more than one listing to page through -
  // every other list card on this build shows exactly one item today (see
  // Aircraft.h's own remarks on why), so this is the first card that has ever
  // needed to tell a household "there is more" at all.
  if (total > 1) {
    lcd.setFont(&fonts::FreeSansBold9pt7b);
    lcd.setTextSize(1);
    lcd.setTextColor(muted(), bg());
    char caption[16];
    snprintf(caption, sizeof(caption), "%u of %u", static_cast<unsigned>(index) + 1,
             static_cast<unsigned>(total));
    lcd.setTextDatum(top_right);
    lcd.drawString(caption, kScreenW - kCardMargin, 4);
    lcd.setTextDatum(top_left);
  }

  // Headline: the address itself, the one fact that actually identifies
  // *this* listing from the last one shown.
  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextSize(1);
  lcd.setTextColor(ink(), bg());
  drawTruncatedLeft(address, kCardMargin, 32, kScreenW - kCardMargin * 2);

  // Price and property type share the sub-headline - the same "two related
  // facts, one line" pairing showAircraftCard() gives callsign+distance.
  lcd.setFont(&fonts::FreeSansBold9pt7b);
  lcd.setTextColor(muted(), bg());
  String subline = formatPrice(price);
  if (propertyType.length() > 0) {
    subline += " - " + propertyType;
  }
  drawTruncatedLeft(subline, kCardMargin, 64, kScreenW - kCardMargin * 2);

  // Stat rows: identical label-left/value-right technique to
  // showAircraftCard()'s Altitude/Speed/Heading block - the same shape of
  // information, a house instead of a plane.
  const int rightX = kScreenW - kCardMargin;
  const int rowValueWidth = 150;
  int rowY = 88;
  constexpr int kRowHeight = 26;

  lcd.setTextColor(muted(), bg());
  lcd.drawString("Beds", kCardMargin, rowY);
  lcd.setTextColor(ink(), bg());
  drawRightJustified(formatCount(bedrooms), rightX, rowY, rowValueWidth);
  rowY += kRowHeight;

  lcd.setTextColor(muted(), bg());
  lcd.drawString("Baths", kCardMargin, rowY);
  lcd.setTextColor(ink(), bg());
  drawRightJustified(formatCount(bathrooms), rightX, rowY, rowValueWidth);
  rowY += kRowHeight;

  lcd.setTextColor(muted(), bg());
  lcd.drawString("Sq Ft", kCardMargin, rowY);
  lcd.setTextColor(ink(), bg());
  drawRightJustified(squareFootage > 0 ? String(squareFootage) : String("-"), rightX, rowY,
                     rowValueWidth);
  rowY += kRowHeight;

  lcd.setTextColor(muted(), bg());
  lcd.drawString("Listed", kCardMargin, rowY);
  lcd.setTextColor(ink(), bg());
  drawRightJustified(formatDaysOnMarket(daysOnMarket), rightX, rowY, rowValueWidth);
  rowY += kRowHeight;

  // Footer: distance and freshness together, both pinned to a fixed baseline
  // below the stat rows rather than flowing under them - same reasoning
  // showWeatherCard()'s own freshness line gives for not letting this move
  // around the card as other fields change length.
  char distanceBuf[32];
  snprintf(distanceBuf, sizeof(distanceBuf), "%.1f mi away", distanceMiles);
  lcd.setFont(&fonts::FreeSansBold9pt7b);
  lcd.setTextSize(1);
  lcd.setTextColor(muted(), bg());
  lcd.setTextDatum(top_left);
  lcd.drawString(distanceBuf, kCardMargin, rowY + 4);
  if (updatedAt.length() > 0) {
    drawRightJustified(updatedAt, rightX, rowY + 4, 160);
  }

  drawClock();
  restoreDefaultFont();
}

void showListingsStatus(const String& headline, const String& detail, bool isProblem) {
  lcd.fillScreen(bg());
  drawCardBanner("LISTINGS", kListingsBanner, 130);

  lcd.setFont(&fonts::FreeSansBold9pt7b);
  lcd.setTextSize(1);
  const uint32_t headlineColour = isProblem ? kWarn : muted();
  const int headlineLines =
      wrappedLeftText(headline, kCardMargin, 40, headlineColour, 22, 3, kScreenW - kCardMargin * 2);
  if (detail.length() > 0) {
    wrappedLeftText(detail, kCardMargin, 40 + headlineLines * 22 + 12, ink(), 18, 3,
                    kScreenW - kCardMargin * 2);
  }

  drawClock();
  restoreDefaultFont();
}

// "Now" for column 0 - it is the same day/period the hero above already
// names in full, and repeating "Today"/"Tonight" in a 60px-wide column would
// either truncate to something unreadable or crowd out the temperature and
// icon beneath it. Columns 1+ get the first three characters of whatever
// name the server sent ("Monday" -> "Mon"), which is as far as this width
// stretches at a size still legible from across a room.
String shortDayLabel(uint8_t index, const String& name) {
  if (index == 0) {
    return "Now";
  }
  return name.length() > 3 ? name.substring(0, 3) : name;
}

// The combined current-plus-outlook layout (see Display.h's own remarks on
// why this replaced both the old per-period paging version of this card and
// the retired showWeatherCard()). Hero at top - icon, temperature, condition
// phrase, freshness - answers "what is it doing right now"; a five-column
// strip below answers "what about the rest of the week", each column its
// own small icon over a compact temperature. Every number below is a fixed
// pixel position, not a computed offset, because unlike the old version this
// card no longer resizes its own body text to fit a phrase of unknown
// length - the strip below has to start at the same y every time regardless
// of how long today's shortForecast happens to be.
void showForecastCard(const String& location, bool currentIsDaytime, int currentTemperature,
                      const String& currentUnit, const String& currentShortForecast,
                      const String* dayNames, const int* dayTemperatures,
                      const String* dayUnits, const String* dayConditions, uint8_t dayCount,
                      const String& updatedAt) {
  lcd.fillScreen(bg());
  drawCardBanner("FORECAST", kForecastBanner, 130);
  const int bodyWidth = kScreenW - kCardMargin * 2;

  // Day/Night, in the banner row's own right-hand corner where this card's
  // old "N of M" paging caption used to sit - there is nothing left to page
  // through now that every period draws on one slide, but the flag itself
  // is still worth a glance: shortForecast for "Tonight" reads differently
  // than the same phrase would for "Today".
  lcd.setFont(&fonts::FreeSansBold9pt7b);
  lcd.setTextSize(1);
  lcd.setTextColor(muted(), bg());
  lcd.setTextDatum(top_right);
  lcd.drawString(currentIsDaytime ? "Day" : "Night", kScreenW - kCardMargin, 4);
  lcd.setTextDatum(top_left);

  // Location, same placement as the retired showWeatherCard()'s own line -
  // this card answers the identical "72 degrees *where*" question that one
  // did.
  if (location.length() > 0) {
    lcd.setTextColor(muted(), bg());
    drawTruncatedLeft(location, kCardMargin, 28, bodyWidth);
  }

  // The hero icon and temperature, side by side - icon on the left the way a
  // household reads "sun, then the number" left to right, at a size (24px
  // radius, 48px across) big enough to tell shapes apart from across a
  // room. The period name itself ("Today"/"Tonight") is not drawn anywhere
  // on this card: it would either duplicate the Day/Night tag above or the
  // "Now" label the strip's own first column already carries.
  constexpr int kHeroIconCx = 40;
  constexpr int kHeroIconCy = 60;
  constexpr int kHeroIconRadius = 24;
  drawWeatherIcon(classifyCondition(currentShortForecast), kHeroIconCx, kHeroIconCy,
                  kHeroIconRadius);

  lcd.setFont(&fonts::FreeSansBold24pt7b);
  lcd.setTextSize(1);
  drawTemperature(currentTemperature, currentUnit, 76, 40,
                  gIsDaytime ? kForecastBanner : ink(), /*ringRadius=*/8);

  // The condition phrase - one truncated line, not the old two-tier wrap.
  // The strip below needs a fixed starting y regardless of how long today's
  // shortForecast is, so unlike the retired showWeatherCard() this cannot
  // grow into a second or third line; a full phrase is still one touch away
  // in the debug stream's own fetch-side log line (see Forecast.cpp).
  if (currentShortForecast.length() > 0) {
    lcd.setFont(&fonts::FreeSansBold12pt7b);
    lcd.setTextSize(1);
    lcd.setTextColor(ink(), bg());
    drawTruncatedLeft(currentShortForecast, 76, 80, kScreenW - 76 - kCardMargin);
  }

  if (updatedAt.length() > 0) {
    lcd.setFont(&fonts::FreeSansBold9pt7b);
    lcd.setTextSize(1);
    lcd.setTextColor(muted(), bg());
    drawTruncatedLeft(updatedAt, kCardMargin, 104, bodyWidth);
  }

  // A thin rule separating "right now" from "the rest of the week" - the
  // only line-art on this card that is not a weather icon.
  lcd.drawFastHLine(kCardMargin, 118, bodyWidth, muted());

  // The five-day strip. Column width divides the panel evenly
  // (kMaxForecastStripDays = 5, so 60px per column with no remainder at
  // kScreenW = 320); each column centres its own day label, icon and
  // temperature independently rather than sharing any x position with its
  // neighbours, so dayCount can be anywhere from 1 to
  // kMaxForecastStripDays without leaving a lopsided gap.
  constexpr int kColumnWidth = (kScreenW - kCardMargin * 2) / kMaxForecastStripDays;
  for (uint8_t i = 0; i < dayCount && i < kMaxForecastStripDays; i++) {
    const int columnCentreX = kCardMargin + i * kColumnWidth + kColumnWidth / 2;

    lcd.setFont(&fonts::FreeSansBold9pt7b);
    lcd.setTextSize(1);
    lcd.setTextColor(muted(), bg());
    lcd.setTextDatum(top_center);
    lcd.drawString(shortDayLabel(i, dayNames[i]), columnCentreX, 122);

    drawWeatherIcon(classifyCondition(dayConditions[i]), columnCentreX, 155, /*radius=*/17);

    char tempBuffer[12];
    snprintf(tempBuffer, sizeof(tempBuffer), "%d%s", dayTemperatures[i], dayUnits[i].c_str());
    lcd.setTextColor(ink(), bg());
    lcd.drawString(tempBuffer, columnCentreX, 178);
    lcd.setTextDatum(top_left);
  }

  drawClock();
  restoreDefaultFont();
}

void showForecastStatus(const String& headline, const String& detail, bool isProblem) {
  lcd.fillScreen(bg());
  drawCardBanner("FORECAST", kForecastBanner, 130);

  lcd.setFont(&fonts::FreeSansBold9pt7b);
  lcd.setTextSize(1);
  const uint32_t headlineColour = isProblem ? kWarn : muted();
  const int headlineLines =
      wrappedLeftText(headline, kCardMargin, 40, headlineColour, 22, 3, kScreenW - kCardMargin * 2);
  if (detail.length() > 0) {
    wrappedLeftText(detail, kCardMargin, 40 + headlineLines * 22 + 12, ink(), 18, 3,
                    kScreenW - kCardMargin * 2);
  }

  drawClock();
  restoreDefaultFont();
}

void showNoContent(const String& headline, const String& detail) {
  lcd.fillScreen(bg());

  lcd.setFont(&fonts::FreeSansBold9pt7b);
  lcd.setTextSize(1);
  const int headlineLines =
      wrappedLeftText(headline, kCardMargin, 60, muted(), 22, 3, kScreenW - kCardMargin * 2);
  if (detail.length() > 0) {
    wrappedLeftText(detail, kCardMargin, 60 + headlineLines * 22 + 12, muted(), 18, 3,
                    kScreenW - kCardMargin * 2);
  }

  drawClock();
  restoreDefaultFont();
}

void actionButtonZone(uint8_t index, uint8_t count, int16_t& x, int16_t& y, int16_t& w,
                      int16_t& h) {
  x = 0;
  y = 0;
  w = 0;
  h = 0;
  if (count == 0 || index >= count) {
    return;
  }
  const int available = kButtonRowRight - kButtonRowLeft;
  const int width = (available - kButtonGap * (count - 1)) / count;
  x = static_cast<int16_t>(kButtonRowLeft + index * (width + kButtonGap));
  y = static_cast<int16_t>(kButtonRowY);
  w = static_cast<int16_t>(width);
  h = static_cast<int16_t>(kButtonHeight);
}

namespace {

// The one place a button's rect, font and text are actually painted, so
// drawActionButtons() can never draw a button a different size or in a
// different font than any other.
void drawOneButton(uint8_t index, uint8_t count, const String& label, uint32_t fill) {
  int16_t x, y, w, h;
  actionButtonZone(index, count, x, y, w, h);
  if (w <= 0) {
    return;
  }

  lcd.fillRoundRect(x, y, w, h, kButtonRadius, fill);
  lcd.setFont(&fonts::FreeSansBold9pt7b);
  lcd.setTextSize(1);
  lcd.setTextColor(kButtonInk, fill);

  // Truncated one character at a time to fit, the same technique
  // drawRightJustified() uses - the label is the server's wording drawn
  // verbatim, and it has no idea how wide this panel is.
  String text = label;
  const int maxTextWidth = w - 10;
  while (text.length() > 1 && lcd.textWidth(text) > maxTextWidth) {
    text = text.substring(0, text.length() - 1);
  }
  lcd.setTextDatum(middle_center);
  lcd.drawString(text, x + w / 2, y + h / 2);
  lcd.setTextDatum(top_left);
}

}  // namespace

void drawActionButtons(const String* labels, uint8_t count) {
  if (labels == nullptr || count == 0) {
    return;
  }
  for (uint8_t i = 0; i < count; ++i) {
    drawOneButton(i, count, labels[i], kButtonFill);
  }
  restoreDefaultFont();
}

void showButtonPressConfirmation() {
  const int cx = kScreenW / 2;
  const int cy = kScreenH / 2;
  const int radius = 46;
  lcd.fillCircle(cx, cy, radius, kConfirmFill);

  // A checkmark as two thick strokes rather than a filled glyph - the same
  // "drawn as lines, not a font" convention drawNavAffordances() already
  // uses for its edge chevrons, just heavier so it stays legible at this
  // size. Several parallel 1px lines stand in for stroke width, since
  // LovyanGFX's drawLine() itself is always 1px.
  const int thickness = 5;
  for (int t = -thickness / 2; t <= thickness / 2; t++) {
    lcd.drawLine(cx - 22, cy + t, cx - 4, cy + 18 + t, kConfirmInk);
    lcd.drawLine(cx - 4, cy + 18 + t, cx + 24, cy - 16 + t, kConfirmInk);
  }

  // Held long enough to actually register as "this happened" - the small
  // per-button flash below is 180ms because it is a tiny accent the user's
  // finger is already resting on; this occupies the centre of the panel
  // and needs a genuinely readable beat before the caller redraws the card
  // out from under it.
  delay(500);
}

void drawNavAffordances(bool canReverse) {
  // Drawn as three lines rather than a filled triangle: at this size a filled
  // arrow reads as a solid blob, and the point of these is to be noticed
  // without being loud.
  const uint32_t reverseColour = canReverse ? muted() : bg();
  const int leftTipX = 5;
  lcd.drawLine(leftTipX + kChevronWidth, kChevronCentreY - kChevronHalfHeight, leftTipX,
               kChevronCentreY, reverseColour);
  lcd.drawLine(leftTipX, kChevronCentreY, leftTipX + kChevronWidth,
               kChevronCentreY + kChevronHalfHeight, reverseColour);

  const int rightTipX = kScreenW - 6;
  lcd.drawLine(rightTipX - kChevronWidth, kChevronCentreY - kChevronHalfHeight, rightTipX,
               kChevronCentreY, muted());
  lcd.drawLine(rightTipX, kChevronCentreY, rightTipX - kChevronWidth,
               kChevronCentreY + kChevronHalfHeight, muted());
}

void flashNavEdge(bool isForward, bool canReverse) {
  // The same kEdgeZoneWidth-wide strip Touch::poll() classifies as
  // Hit::Reverse/Hit::Forward - see that constant's own remarks above - but
  // only up to kButtonRowY, not the full kScreenH. Touch::poll() checks
  // action-button zones before edge zones, so within the button row's own
  // y-range a tap only ever reaches Hit::Reverse/Hit::Forward where no
  // button rect covers it; a taller flash here would still paint over
  // whatever button *does* live in that row at that x, and unlike
  // showButtonPressConfirmation() (always followed by drawCurrent()
  // redrawing everything, since a nav tap changes what's on screen) there
  // is one path - rewind() at gHistoryCursor == 0, i.e. canReverse == false
  // - that returns without redrawing at all, which would leave a bite
  // taken out of a real button until some unrelated later redraw happened
  // to fix it.
  const int x = isForward ? kScreenW - kEdgeZoneWidth : 0;
  // kButtonPressedFill, not a new colour: reusing the action row's own
  // "pressed" shade makes every touch on this panel answer back the same
  // way, rather than teaching the user two different flash colours for two
  // different kinds of button. It reads against either day/night background
  // for the same reason it was chosen for the action row in the first place.
  lcd.fillRect(x, 0, kEdgeZoneWidth, kButtonRowY, kButtonPressedFill);
  delay(180);
  // Undo the flash before redrawing the chevron on top of it - the resting
  // state here isn't a filled rect the way a button's is, so simply drawing
  // the chevron again over the pressed fill would leave a stray blue bar
  // behind it.
  lcd.fillRect(x, 0, kEdgeZoneWidth, kButtonRowY, bg());
  drawNavAffordances(canReverse);
}

namespace {

/// The whole file's bytes, read from SD in one pass, or a null `data` on any
/// failure (open, zero-length, short read).
struct FileBuffer {
  std::unique_ptr<uint8_t[]> data;
  size_t size = 0;
};

/// Reads `path` entirely into a heap buffer before either PNG draw function
/// below ever calls into LovyanGFX's decoder - see README.md's "The likely
/// root cause" section: this board's SD card shares its SPI wiring with the
/// display, by the manufacturer's own documentation, and `drawPngFile()`'s
/// own file-streaming decode interleaves an SD read with every display
/// write for the entire length of the image, landing squarely on that
/// shared bus for as long as the decode runs. Reading the whole file first
/// means the SD read and every one of the display writes that follow are
/// separated in time - the SD card is never touched again once this
/// returns - even though they still share the same physical wire. The
/// largest asset in the catalog today is under 34KB against roughly 250KB
/// of free heap in the device's ordinary operating state, so this is a
/// one-shot allocation freed the moment the caller returns, not a standing
/// cost.
FileBuffer readFileToBuffer(const String& path) {
  FileBuffer result;
  File file = SD.open(path, FILE_READ);
  if (!file) {
    Log::printf("[display] could not open %s to read into memory", path.c_str());
    return result;
  }
  const size_t fileSize = file.size();
  if (fileSize == 0) {
    file.close();
    Log::printf("[display] %s is empty on SD - nothing to read into memory", path.c_str());
    return result;
  }
  std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[fileSize]);
  if (!buffer) {
    file.close();
    Log::printf("[display] out of memory reading %s (%u bytes)", path.c_str(),
                static_cast<unsigned>(fileSize));
    return result;
  }
  const size_t bytesRead = file.read(buffer.get(), fileSize);
  file.close();
  if (bytesRead != fileSize) {
    Log::printf("[display] short read on %s (%u of %u bytes)", path.c_str(),
                static_cast<unsigned>(bytesRead), static_cast<unsigned>(fileSize));
    return result;
  }
  result.data = std::move(buffer);
  result.size = fileSize;
  Log::printf("[display] read %s into memory (%u bytes) - SD access done, decoding from RAM now",
              path.c_str(), static_cast<unsigned>(fileSize));
  return result;
}

}  // namespace

bool drawPngFromSdInRect(const String& path, int32_t x, int32_t y, int32_t w, int32_t h) {
  // No fillScreen() here, deliberately - see this function's own header
  // comment. Same decode call as drawPngFromSd() below, just bounded to
  // (w, h) at (x, y) instead of the whole panel; scaleX/scaleY left at 0
  // is what makes LovyanGFX auto-fit the image within that box rather than
  // drawing it at native size.
  const FileBuffer file = readFileToBuffer(path);
  if (!file.data) {
    Log::printf("[display] could not read %s for a %dx%d rect at (%d,%d)", path.c_str(),
                (int)w, (int)h, (int)x, (int)y);
    return false;
  }
  const bool ok =
      lcd.drawPng(file.data.get(), file.size, x, y, w, h, 0, 0, 0.0f, 0.0f, middle_center);
  lcd.releasePngMemory();
  if (!ok) {
    Log::printf("[display] failed to draw %s in %dx%d rect at (%d,%d)", path.c_str(), (int)w, (int)h, (int)x, (int)y);
  }
  return ok;
}

bool drawPngFromSd(const String& path) {
  lcd.fillScreen(bg());
  const FileBuffer file = readFileToBuffer(path);
  bool ok = false;
  if (!file.data) {
    Log::printf("[display] could not read %s", path.c_str());
  } else {
    ok = lcd.drawPng(file.data.get(), file.size, 0, 0, 0, 0, 0, 0, 0.0f, 0.0f, middle_center);
    // LovyanGFX keeps the PNG decoder's internal buffers allocated after a
    // draw (intentional, for cheap repeat-draws). Released unconditionally,
    // because a *failed* decode leaves them allocated too - CYD-Dickey found
    // this starving the memory its Bluetooth init needed immediately
    // afterwards, and this device has roughly 274KB of free heap to lose it
    // out of.
    lcd.releasePngMemory();
    if (!ok) {
      Log::printf("[display] failed to draw %s", path.c_str());
    }
  }
  restoreDefaultFont();
  return ok;
}

}  // namespace Display
