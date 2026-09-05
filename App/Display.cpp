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
// now occupies roughly y 222-236, x 265-314. kButtonRowY/kButtonHeight are
// therefore left exactly as they were: a row ending at y=220 clears the
// clock by 2px, and starting at y=190 clears the card content above by a
// similar margin, and neither number moves without the other three
// (content layout, clock size, clock position) being reconsidered together.
//
// Width is a different story. The touch controller checks action-button
// zones before the reverse/forward edge strips (see Touch.cpp's poll() and
// its own remarks on why that ordering is fixed) - a tap landing inside a
// button rect is always a button press, regardless of how close that rect
// sits to the physical edge. So unlike the 16px edge strips themselves,
// which have to stay clear of card content in the middle of the screen, a
// button row has nothing logical to stay clear of horizontally, and can run
// almost the full 320px width. Real fingers found the old 24px-to-296px row
// (272px total, narrowing to 85px for a three-button card) too narrow to hit
// reliably; it now runs 8px to 312px (304px total, 97px for three buttons -
// roughly +14% per button), with a small residual margin from the true
// bezel edge kept only because a resistive panel's accuracy is known to
// degrade right at the glass edge, not because anything would misfire.
constexpr int kButtonRowY = 190;
constexpr int kButtonHeight = 30;
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

// Layout note, because this card was restyled twice and the second pass is
// the one that matters.
//
// The first pass matched CYD-Dickey's *elements* - navy banner, bold
// left-aligned temperature, left-margined body - and was still reported as
// worse than the original on real hardware. Reading the two side by side
// explains why, and it is not a detail either version got wrong: their card
// carries five live readings plus a five-day strip (temperature, condition,
// feels-like, humidity, wind, then M/D + high/low for five days) and fills
// the panel top to bottom with them. This card has three fields to show,
// because that is all /api/myweather/mine sends (see Weather.h). Copying a
// dense layout's type sizes onto a third of its content produced a card that
// was small AND empty - roughly 70 vertical pixels of content on a 240px
// panel, with everything below y=120 blank.
//
// So this pass stops imitating their density and spends the space instead.
// Fewer facts, set larger: the temperature becomes a genuine hero number at
// 24pt rather than sharing 12pt with everything else, and the two lines that
// were 6x8 bitmap grey are set in the same readable bold 9pt the clock
// settled on. What is deliberately NOT done here is padding the empty space
// with invented content - no fake humidity, no placeholder forecast strip.
// The gap is real and it is server-side; see README.
void showWeatherCard(const String& location, int temperature, const String& unit,
                     const String& shortForecast, const String& updatedAt) {
  lcd.fillScreen(bg());
  drawCardBanner("WEATHER", kWeatherBanner, 110);

  // Location on its own line directly under the banner, on the same left
  // column as everything else on the card. CYD-Dickey's weather card has no
  // location field at all (it assumes local weather); CAL's data carries one
  // (Home or Target's city/state), so it stays - it is the difference between
  // "72 degrees" and "72 degrees *where*", which matters precisely because
  // this device may be sitting somewhere other than the address it reports.
  //
  // It previously sat right-justified on the banner row in Font0 - the 6x8
  // bitmap font, in muted grey. That is the exact combination drawClock()
  // above records as having failed on real hardware: roughly 3mm tall on this
  // 2.8" panel, low contrast, read from across a room, and reported by the
  // first person who saw it as simply not being there. The clock was fixed
  // at the time; these two micro-text lines on the same card had the identical
  // defect for the identical reason and were not. Both are now set in that
  // same bold 9pt face.
  if (location.length() > 0) {
    lcd.setFont(&fonts::FreeSansBold9pt7b);
    lcd.setTextSize(1);
    lcd.setTextColor(muted(), bg());
    drawTruncatedLeft(location, kCardMargin, 28, kScreenW - kCardMargin * 2);
  }

  // The hero number. CYD-Dickey sets its temperature at 12pt because it is
  // one of six things competing for the same panel; here it is one of three,
  // so it gets the weight that buys. Tinted with the weather banner's own
  // navy by day - it reads fine against the white day background, and echoes
  // the banner colour the way CYD-Dickey's own card doesn't bother to. That
  // same navy would be nearly invisible against the night background
  // (dark-on-black), so night falls back to the plain theme ink colour
  // instead - the banner rect above still carries the navy accent either way,
  // so nothing brand-identifying is lost at night.
  //
  // 24pt digits are 35px tall, so this block occupies y 50-85 and the ring
  // scales to match (see drawTemperature). Widest realistic string, a
  // three-digit temperature, ends around x=150 - nowhere near the right edge.
  lcd.setFont(&fonts::FreeSansBold24pt7b);
  lcd.setTextSize(1);
  drawTemperature(temperature, unit, kCardMargin, 50, gIsDaytime ? kWeatherBanner : ink(),
                  /*ringRadius=*/8);

  // The condition phrase. CYD-Dickey never wraps here because its
  // weatherCodeDescription() is always a word or two ("Overcast"); NWS's
  // shortForecast is a whole phrase ("Chance Showers And Thunderstorms then
  // Partly Sunny"), so this picks the largest size the phrase actually fits
  // in rather than clipping it: 12pt while it lands in two lines or fewer,
  // dropping to 9pt and three lines when it does not. A forecast the
  // household can read in full at a smaller size beats half a forecast at a
  // larger one, and truncating mid-phrase can invert the meaning of exactly
  // the sentences worth reading ("...then Clearing").
  const int bodyWidth = kScreenW - kCardMargin * 2;
  if (shortForecast.length() > 0) {
    lcd.setFont(&fonts::FreeSansBold12pt7b);
    lcd.setTextSize(1);
    const int linesAtLargeSize = wrappedLeftText(shortForecast, kCardMargin, 100, ink(), 24, 3,
                                                 bodyWidth, /*measureOnly=*/true);
    if (linesAtLargeSize <= 2) {
      wrappedLeftText(shortForecast, kCardMargin, 100, ink(), 24, 2, bodyWidth);
    } else {
      lcd.setFont(&fonts::FreeSansBold9pt7b);
      wrappedLeftText(shortForecast, kCardMargin, 100, ink(), 18, 3, bodyWidth);
    }
  }

  // Freshness, pinned to a fixed baseline rather than flowing under whatever
  // the condition block happened to need. Both branches above bottom out
  // above this line (12pt x 2 = y148, 9pt x 3 = y154), and a fixed position
  // means this line does not jump around the card every time the forecast
  // wording changes length - the card is looked at from across a room, where
  // a moving element is read as a change in the data.
  //
  // The caller computes the wording. It is worth saying plainly that this
  // used to be the hardcoded string "Updated just now" on every draw,
  // including redraws of a card fetched twenty minutes earlier - see
  // Weather.cpp, which now measures it.
  if (updatedAt.length() > 0) {
    lcd.setFont(&fonts::FreeSansBold9pt7b);
    lcd.setTextSize(1);
    lcd.setTextColor(muted(), bg());
    drawTruncatedLeft(updatedAt, kCardMargin, 162, bodyWidth);
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
    // 5 lines at 24px is y 40-160, clear of the button row that starts at
    // y=190 (see this file's own remarks on kButtonRowY further up).
    const int linesAtLargeSize =
        wrappedLeftText(text, kCardMargin, 40, ink(), 24, 5, bodyWidth, /*measureOnly=*/true);
    if (linesAtLargeSize <= 5) {
      wrappedLeftText(text, kCardMargin, 40, ink(), 24, 5, bodyWidth);
    } else {
      // 7 lines at 18px is y 40-166, same clearance at the smaller size - and
      // 7 lines of roughly 38 characters each comfortably covers the full
      // 280-character limit without a further fallback tier.
      lcd.setFont(&fonts::FreeSansBold9pt7b);
      wrappedLeftText(text, kCardMargin, 40, ink(), 18, 7, bodyWidth);
    }
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

bool drawPngFromSdInRect(const String& path, int32_t x, int32_t y, int32_t w, int32_t h) {
  // No fillScreen() here, deliberately - see this function's own header
  // comment. Same decode call as drawPngFromSd() below, just bounded to
  // (w, h) at (x, y) instead of the whole panel; scaleX/scaleY left at 0
  // is what makes LovyanGFX auto-fit the image within that box rather than
  // drawing it at native size.
  const bool ok = lcd.drawPngFile(SD, path.c_str(), x, y, w, h, 0, 0, 0.0f, 0.0f, middle_center);
  lcd.releasePngMemory();
  if (!ok) {
    Log::printf("[display] failed to draw %s in %dx%d rect at (%d,%d)", path.c_str(), (int)w, (int)h, (int)x, (int)y);
  }
  return ok;
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
