// SD.h MUST come before LovyanGFX.hpp, not after. LovyanGFX auto-detects
// SD-card image support by checking whether the SD library's own include
// guard is already defined (see its esp32/common.hpp); include it afterwards
// and drawPngFile(SD, ...) fails to compile with "abstract type
// DataWrapperT<fs::SDFS>". CYD-Dickey hit exactly this and records the same
// note at the top of its .ino.
#include <SD.h>

#define LGFX_AUTODETECT
#include <LovyanGFX.hpp>
#include <LGFX_AUTODETECT.hpp>
#include <time.h>

#include "Display.h"
#include "Log.h"

namespace Display {
namespace {

// Same panel as CAL - see CAL's own Display.cpp for why autodetect over a pin
// map. No CalQr.h here: the App never renders a QR code.
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
// the banner rects below (kWeatherBanner/kAircraftBanner) are their own
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

// CYD-Dickey's WEATHER banner is navy (fillRect + TFT_NAVY); aircraft has no
// banner of its own there (its card spends that space on an airline logo
// CAL has no equivalent data for - see Aircraft.h), so this colour is new,
// chosen only to read as visually distinct from weather's on the same
// device.
constexpr uint32_t kWeatherBanner = 0x0D2B52u;
constexpr uint32_t kAircraftBanner = 0x1F6FEBu;
// Amber, distinct from the two blues above so the three cards are told apart at
// a glance from across a room rather than by reading the banner text.
constexpr uint32_t kSunMoonBanner = 0xB45309u;
constexpr int kBannerHeight = 22;
constexpr int kCardMargin = 10;

// Card chrome geometry - see Display.h's own remarks on why all of it is
// decided here rather than described by the server.
//
// The button row has to stay clear of three things at once: the 16px
// left/right edge strips Touch.h uses for reverse/forward, and the corner
// clock, which drawClock() sets bottom-right at (314, 236). The clock was
// enlarged to FreeSans9pt after it proved invisible on real hardware at its
// original 6x8 bitmap size, so it now occupies roughly y 222-236, x 265-314.
// A row from x=24 to x=296 ending at y=220 still clears it, but the vertical
// gap is now 2px rather than 8 - so growing the clock again, or moving the
// button row down, needs both numbers reconsidered together rather than one
// of them nudged in isolation.
constexpr int kButtonRowY = 190;
constexpr int kButtonHeight = 30;
constexpr int kButtonRowLeft = 24;
constexpr int kButtonRowRight = 296;
constexpr int kButtonGap = 8;
constexpr int kButtonRadius = 6;

// The same bright, high-contrast blue CYD-Dickey settled on for its own
// buttons (its BUTTON_COLOR = 0x2E9FFF), chosen there because the default
// dark navy was hard to read on this panel. Deliberately outside the
// day/night swap for the same reason the banner colours are: it reads
// against either background, and white-on-blue stays legible either way.
constexpr uint32_t kButtonFill = 0x2E9FFFu;
constexpr uint32_t kButtonPressedFill = 0x0B5FB0u;
constexpr uint32_t kButtonInk = 0xFFFFFFu;

// Edge chevrons. Small, low-contrast, vertically centred - they mark the
// touch zones without competing with the card for attention.
constexpr int kChevronHalfHeight = 12;
constexpr int kChevronWidth = 7;
constexpr int kChevronCentreY = kScreenH / 2;

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
int wrappedLeftText(const String& text, int x, int y, uint32_t colour, int lineHeight,
                    int maxLines, int maxWidth) {
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
        lcd.drawString(candidate, x, cursorY);
        linesDrawn++;
      }
      continue;
    }

    const int breakAt = (lastSpace > lineStart) ? lastSpace : i;
    lcd.drawString(text.substring(lineStart, breakAt), x, cursorY);
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

// The hand-drawn-degree-ring technique from the original centeredTemperature
// above, adapted to a left-aligned origin instead of screen-centered - this
// restyle's cards lay out left-margined (see wrappedLeftText's remarks), so
// the temperature moves to match rather than staying centered on its own.
// The bold GFX font used here (fonts::FreeSansBold12pt7b, set by the
// caller) is exactly as ASCII-only as the bitmap font the original comment
// describes - the missing-glyph problem, and the reason for drawing this
// ring instead of a literal degree character, applies here unchanged.
void drawTemperature(int temperature, const String& unit, int x, int y, uint32_t colour) {
  lcd.setTextColor(colour, bg());
  lcd.setTextDatum(top_left);

  const String numberText = String(temperature);
  int cursorX = x;
  lcd.drawString(numberText, cursorX, y);
  cursorX += lcd.textWidth(numberText);

  constexpr int kRadius = 5;
  constexpr int kGap = 5;
  cursorX += kGap;
  // Near the top of the glyph's cap-height, like a real superscript degree
  // mark - same placement rationale as the original, just against this
  // font's taller cap-height.
  lcd.drawCircle(cursorX + kRadius, y + kRadius, kRadius, colour);
  cursorX += kRadius * 2 + kGap;

  lcd.drawString(unit, cursorX, y);
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

void showWeatherCard(const String& location, int temperature, const String& unit,
                     const String& shortForecast, const String& updatedAt) {
  lcd.fillScreen(bg());
  drawCardBanner("WEATHER", kWeatherBanner, 110);

  // Location, right-justified against the card's right margin on the same
  // row as the banner - CYD-Dickey's own weather card has no location line
  // (it assumes local weather), but CAL's server data carries one
  // (Home/Target city, state) and dropping it would lose real information
  // the original centered card showed.
  if (location.length() > 0) {
    lcd.setFont(&fonts::Font0);
    lcd.setTextSize(1);
    lcd.setTextColor(muted(), bg());
    drawRightJustified(location, kScreenW - 8, 7, kScreenW - 120);
  }

  // Big left-aligned temperature in a bold sans font - CYD-Dickey's
  // `lcd.setFont(&fonts::FreeSansBold12pt7b); ...; lcd.printf("%.0fF\n")` at
  // (10, 32), minus their plain "F" (see drawTemperature's own remarks on
  // why the hand-drawn ring stays). Tinted with the weather banner's own
  // navy by day - it reads fine against the white day background, and
  // echoes the banner colour the way CYD-Dickey's own card doesn't bother
  // to. That same navy would be nearly invisible against the night
  // background (dark-on-black), so night falls back to the plain theme ink
  // colour instead - the banner rect above still carries the navy accent
  // either way, so nothing brand-identifying is actually lost at night.
  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextSize(1);
  drawTemperature(temperature, unit, kCardMargin, 32, gIsDaytime ? kWeatherBanner : ink());

  // Short forecast, bold sans, left-margined and word-wrapped underneath -
  // same position CYD-Dickey uses for its weatherCodeDescription() line
  // relative to the temperature above it, just wrapped instead of a single
  // println() since shortForecast can run considerably longer than "Overcast".
  lcd.setFont(&fonts::FreeSansBold9pt7b);
  lcd.setTextSize(1);
  const int forecastLines =
      wrappedLeftText(shortForecast, kCardMargin, 78, ink(), 22, 4, kScreenW - kCardMargin * 2);

  if (updatedAt.length() > 0) {
    lcd.setFont(&fonts::Font0);
    lcd.setTextSize(1);
    lcd.setTextColor(muted(), bg());
    lcd.setTextDatum(top_left);
    lcd.drawString(updatedAt, kCardMargin, 78 + forecastLines * 22 + 10);
  }

  drawClock();
  restoreDefaultFont();
}

void showWeatherStatus(const String& headline, const String& detail, bool isProblem) {
  lcd.fillScreen(bg());
  drawCardBanner("WEATHER", kWeatherBanner, 110);

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

void showAircraftCard(const String& callsign, int altitudeFeet, double speedKnots,
                      double headingDegrees, double distanceMiles, const String& updatedAt) {
  lcd.fillScreen(bg());
  drawCardBanner("OVERHEAD", kAircraftBanner, 130);

  // Callsign as the card's headline, in the spot CYD-Dickey's
  // drawFeaturedAircraft() gives the airline logo or bold airline name - the
  // most identifying single piece of data available from what
  // DiscoverAroundMe's adsb.lol integration actually returns (see
  // Aircraft.h's own remarks on what CYD-Dickey's version assumes that this
  // server doesn't provide).
  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextSize(1);
  lcd.setTextColor(ink(), bg());
  lcd.setTextDatum(top_left);
  lcd.drawString(callsign, kCardMargin, 32);

  lcd.setFont(&fonts::FreeSansBold9pt7b);
  char distanceBuf[24];
  snprintf(distanceBuf, sizeof(distanceBuf), "%.1f mi away", distanceMiles);
  lcd.setTextColor(muted(), bg());
  lcd.drawString(distanceBuf, kCardMargin, 64);

  // Stat rows: a muted label on the left, the value right-justified against
  // the card's right margin - the same truncate-and-right-justify technique
  // as CYD-Dickey's drawFeaturedAircraft()/drawTruncatedRight (see
  // drawRightJustified above), applied per-row here instead of to a whole
  // second column of airline-specific fields CAL doesn't have data for.
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

  if (updatedAt.length() > 0) {
    lcd.setFont(&fonts::Font0);
    lcd.setTextSize(1);
    lcd.setTextColor(muted(), bg());
    lcd.setTextDatum(top_left);
    lcd.drawString(updatedAt, kCardMargin, rowY + 8);
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

  if (detail.length() > 0) {
    lcd.setFont(&fonts::FreeSansBold9pt7b);
    lcd.setTextColor(muted(), bg());
    wrappedLeftText(detail, kCardMargin, 140, muted(), 20, 2, kScreenW - kCardMargin * 2);
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

// Shared by drawActionButtons() and flashActionButton() so a pressed button
// can never come back a different size or in a different font than the one
// it replaced.
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

void flashActionButton(uint8_t index, uint8_t count, const String& label) {
  drawOneButton(index, count, label, kButtonPressedFill);
  delay(180);
  drawOneButton(index, count, label, kButtonFill);
  restoreDefaultFont();
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

bool drawPngFromSd(const String& path) {
  lcd.fillScreen(bg());
  const bool ok =
      lcd.drawPngFile(SD, path.c_str(), 0, 0, 0, 0, 0, 0, 0.0f, 0.0f, middle_center);
  // LovyanGFX keeps the PNG decoder's internal buffers allocated after a draw
  // (intentional, for cheap repeat-draws). Released unconditionally, because
  // a *failed* decode leaves them allocated too - CYD-Dickey found this
  // starving the memory its Bluetooth init needed immediately afterwards, and
  // this device has roughly 274KB of free heap to lose it out of.
  lcd.releasePngMemory();
  if (!ok) {
    Log::printf("[display] failed to draw %s", path.c_str());
  }
  restoreDefaultFont();
  return ok;
}

}  // namespace Display
