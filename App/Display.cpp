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
constexpr uint32_t kWarn = 0xEDA100u;

// The content-card palette, kept separate from kBg/kInk above: those two
// stay black-on-white for the boot-ladder screens (showStatus/showFailure),
// which WifiJoin and CAL's own Provisioning module both assume look a
// particular way (see Display.h's remarks) and which this restyle
// deliberately leaves alone. Cards themselves switch to CYD-Dickey's actual
// look - every one of its cards (Weather, aircraft, listings, QR, branding)
// is a white background with a small colour-banded label in the corner,
// black body text and a muted grey for secondary lines - rather than CAL's
// original black weather card.
constexpr uint32_t kCardBg = 0xFFFFFFu;
constexpr uint32_t kCardInk = 0x000000u;
constexpr uint32_t kCardMuted = 0x707070u;
// CYD-Dickey's WEATHER banner is navy (fillRect + TFT_NAVY); aircraft has no
// banner of its own there (its card spends that space on an airline logo
// CAL has no equivalent data for - see Aircraft.h), so this colour is new,
// chosen only to read as visually distinct from weather's on the same
// device.
constexpr uint32_t kWeatherBanner = 0x0D2B52u;
constexpr uint32_t kAircraftBanner = 0x1F6FEBu;
constexpr int kBannerHeight = 22;
constexpr int kCardMargin = 10;

void clear() {
  lcd.fillScreen(kBg);
}

// The boot-ladder screens' (showStatus/showFailure) own word-wrap - greedy,
// measured with real font metrics, since server-supplied strings (a card's
// shortForecast, a content-gate refusal message) arrive with no length this
// file controls. See wrappedLeftText further down for the card-layout
// counterpart this restyle adds alongside it.
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
  lcd.setTextColor(colour, kCardBg);
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
  lcd.setTextColor(kInk, bannerColour);
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
  lcd.setTextColor(colour, kCardBg);
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
  lcd.fillScreen(kCardBg);
  drawCardBanner("WEATHER", kWeatherBanner, 110);

  // Location, right-justified against the card's right margin on the same
  // row as the banner - CYD-Dickey's own weather card has no location line
  // (it assumes local weather), but CAL's server data carries one
  // (Home/Target city, state) and dropping it would lose real information
  // the original centered card showed.
  if (location.length() > 0) {
    lcd.setFont(&fonts::Font0);
    lcd.setTextSize(1);
    lcd.setTextColor(kCardMuted, kCardBg);
    drawRightJustified(location, kScreenW - 8, 7, kScreenW - 120);
  }

  // Big left-aligned temperature in a bold sans font - CYD-Dickey's
  // `lcd.setFont(&fonts::FreeSansBold12pt7b); ...; lcd.printf("%.0fF\n")` at
  // (10, 32), minus their plain "F" (see drawTemperature's own remarks on
  // why the hand-drawn ring stays).
  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextSize(1);
  drawTemperature(temperature, unit, kCardMargin, 32, kWeatherBanner);

  // Short forecast, bold sans, left-margined and word-wrapped underneath -
  // same position CYD-Dickey uses for its weatherCodeDescription() line
  // relative to the temperature above it, just wrapped instead of a single
  // println() since shortForecast can run considerably longer than "Overcast".
  lcd.setFont(&fonts::FreeSansBold9pt7b);
  lcd.setTextSize(1);
  const int forecastLines =
      wrappedLeftText(shortForecast, kCardMargin, 78, kCardInk, 22, 4, kScreenW - kCardMargin * 2);

  if (updatedAt.length() > 0) {
    lcd.setFont(&fonts::Font0);
    lcd.setTextSize(1);
    lcd.setTextColor(kCardMuted, kCardBg);
    lcd.setTextDatum(top_left);
    lcd.drawString(updatedAt, kCardMargin, 78 + forecastLines * 22 + 10);
  }

  restoreDefaultFont();
}

void showWeatherStatus(const String& headline, const String& detail, bool isProblem) {
  lcd.fillScreen(kCardBg);
  drawCardBanner("WEATHER", kWeatherBanner, 110);

  lcd.setFont(&fonts::FreeSansBold9pt7b);
  lcd.setTextSize(1);
  const uint32_t headlineColour = isProblem ? kWarn : kCardMuted;
  const int headlineLines =
      wrappedLeftText(headline, kCardMargin, 40, headlineColour, 22, 3, kScreenW - kCardMargin * 2);
  if (detail.length() > 0) {
    wrappedLeftText(detail, kCardMargin, 40 + headlineLines * 22 + 12, kCardInk, 18, 3,
                    kScreenW - kCardMargin * 2);
  }

  restoreDefaultFont();
}

void showAircraftCard(const String& callsign, int altitudeFeet, double speedKnots,
                      double headingDegrees, double distanceMiles, const String& updatedAt) {
  lcd.fillScreen(kCardBg);
  drawCardBanner("OVERHEAD", kAircraftBanner, 130);

  // Callsign as the card's headline, in the spot CYD-Dickey's
  // drawFeaturedAircraft() gives the airline logo or bold airline name - the
  // most identifying single piece of data available from what
  // DiscoverAroundMe's adsb.lol integration actually returns (see
  // Aircraft.h's own remarks on what CYD-Dickey's version assumes that this
  // server doesn't provide).
  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextSize(1);
  lcd.setTextColor(kCardInk, kCardBg);
  lcd.setTextDatum(top_left);
  lcd.drawString(callsign, kCardMargin, 32);

  lcd.setFont(&fonts::FreeSansBold9pt7b);
  char distanceBuf[24];
  snprintf(distanceBuf, sizeof(distanceBuf), "%.1f mi away", distanceMiles);
  lcd.setTextColor(kCardMuted, kCardBg);
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

  lcd.setTextColor(kCardMuted, kCardBg);
  lcd.drawString("Altitude", kCardMargin, rowY);
  lcd.setTextColor(kCardInk, kCardBg);
  drawRightJustified(String(altitudeFeet) + " ft", rightX, rowY, rowValueWidth);
  rowY += kRowHeight;

  lcd.setTextColor(kCardMuted, kCardBg);
  lcd.drawString("Speed", kCardMargin, rowY);
  lcd.setTextColor(kCardInk, kCardBg);
  drawRightJustified(String(static_cast<int>(speedKnots + 0.5)) + " kts", rightX, rowY, rowValueWidth);
  rowY += kRowHeight;

  lcd.setTextColor(kCardMuted, kCardBg);
  lcd.drawString("Heading", kCardMargin, rowY);
  lcd.setTextColor(kCardInk, kCardBg);
  drawRightJustified(compassDirection(headingDegrees), rightX, rowY, rowValueWidth);
  rowY += kRowHeight;

  if (updatedAt.length() > 0) {
    lcd.setFont(&fonts::Font0);
    lcd.setTextSize(1);
    lcd.setTextColor(kCardMuted, kCardBg);
    lcd.setTextDatum(top_left);
    lcd.drawString(updatedAt, kCardMargin, rowY + 8);
  }

  restoreDefaultFont();
}

void showAircraftStatus(const String& headline, const String& detail, bool isProblem) {
  lcd.fillScreen(kCardBg);
  drawCardBanner("OVERHEAD", kAircraftBanner, 130);

  lcd.setFont(&fonts::FreeSansBold9pt7b);
  lcd.setTextSize(1);
  const uint32_t headlineColour = isProblem ? kWarn : kCardMuted;
  const int headlineLines =
      wrappedLeftText(headline, kCardMargin, 40, headlineColour, 22, 3, kScreenW - kCardMargin * 2);
  if (detail.length() > 0) {
    wrappedLeftText(detail, kCardMargin, 40 + headlineLines * 22 + 12, kCardInk, 18, 3,
                    kScreenW - kCardMargin * 2);
  }

  restoreDefaultFont();
}

}  // namespace Display
