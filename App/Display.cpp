// SD.h MUST come before LovyanGFX.hpp, not after. LovyanGFX auto-detects
// SD-card image support by checking whether the SD library's own include
// guard is already defined (see its esp32/common.hpp); include it afterwards
// and drawPngFile(SD, ...) fails to compile with "abstract type
// DataWrapperT<fs::SDFS>". CYD-Dickey hit exactly this and records the same
// note at the top of its .ino.
//
// This one include order covers drawJpgFile(SD, ...) as well, which is worth
// stating since the JPEG path was added later and the constraint looks like it
// might need checking twice. It does not: the gate is a single `#if defined
// (_SD_H_)` in the library's esp32/common.hpp, which defines
// LGFX_FILESYSTEM_SD and with it the one DataWrapperT<fs::SDFS> specialisation
// that every drawXxxFile(SD, ...) overload instantiates. PNG, JPEG, BMP and QOI
// are generated from the same LGFX_FUNCTION_GENERATOR macro (LGFXBase.hpp:922)
// and are gated identically - there is no separate per-format JPEG switch to
// find, and nothing to do here beyond what this include already does.
#include <SD.h>
#include <cstdlib>
// strlen/strncat, for layoutText()'s fixed line buffer. Named explicitly rather
// than leaned on through whatever Arduino.h happens to drag in, because the
// whole point of that buffer is that this file does its own bounded string work
// instead of reaching for Arduino String on the draw path.
#include <cstring>
// For the per-capability heap figures logHeapSnapshot() reports. ESP's own
// wrappers are not usable for this: at one measured instant ESP.getFreeHeap()
// said 49,960 and ESP.getMaxAllocHeap() said 32,756 while the real 8BIT free
// size was 11,340 with a largest block of 6,132.
#include <esp_heap_caps.h>

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

/// Whether every time on this device reads "2:30 PM" rather than "14:30".
///
/// Resolved per device through the config cascade and pushed on each check-in,
/// like the UTC offset beside it - it is a household preference, not a firmware
/// build. Defaults to false so a device that has never checked in, or one whose
/// account has never set it, looks exactly as it did before this existed.
bool gUse12HourClock = false;
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
// A deep plum, chosen by elimination against every banner already here: not
// the notice's green, not listings' brick, not homevalue's bronze, not
// sunmoon's amber, and far enough from moonphase's indigo to survive being
// read across a room. A household's own appointments are the most personal
// thing this device shows, and the one card whose banner must not be mistaken
// for a system notice - which is exactly what it was doing before this
// existed, because Calendar.cpp had no entry point of its own and borrowed
// showAnnouncementCard()'s green "NOTICE".
constexpr uint32_t kCalendarBanner = 0x6D2E6Bu;
// A muted bronze/gold, distinct from every banner above it including
// listings' brick-red and sunmoon's burnt-orange amber - this card's subject
// is literally money, so it earns its own "value" hue rather than borrowing
// either of the two nearest-in-warmth colours already claimed by an
// unrelated real-estate or astronomy card.
constexpr uint32_t kHomeValueBanner = 0x8A6D1Bu;
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
// A deep crimson, picked by elimination against every banner above rather than
// for any association with sport. The obvious choice was a teal-green, and it
// was wrong: tides already own 0x0F6B5C and a sports banner near it would have
// been within a few values per channel - indistinguishable across a room, which
// is the test this palette has been applied with throughout. Crimson is the
// nearest genuinely unclaimed hue, far enough from listings' brick (0xA13D2D)
// on the magenta side and from calendar's plum (0x6D2E6B) on the red side.
constexpr uint32_t kSportsBanner = 0xA3195Bu;
constexpr int kBannerHeight = 22;
constexpr int kCardMargin = 10;

// The Banner/Banner Button themes' own header strip - see showBannerCard()
// below. A warm amber-red, deliberately louder than every showCardBanner()
// label colour above: those are a small corner tag on an otherwise ordinary
// card, where this strip IS the card's entire reason for existing, so it
// earns a colour that reads as "notice me" the way roadside signage does,
// not a quiet corner label. Outside the day/night swap for the same reason
// the button/confirmation colours are: it has to read against either
// background sitting behind whatever a caller draws next.
constexpr uint32_t kBannerStripFill = 0xC2410Cu;
constexpr uint32_t kBannerStripInk = 0xFFFFFFu;
// Tall enough for three wrapped lines of the strip's own 12pt bold font
// (three lines at 26px plus margin - see showBannerCard()) while still
// leaving the strip read as a partial-screen band rather than the whole
// panel - kButtonRowY (160) minus this is the empty "not full-screen"
// clearance a Banner theme's own visual signature depends on.
constexpr int kBannerStripHeight = 100;

// showAircraftCard()'s route line height, in its own constant rather than a
// literal 18 at each of the two call sites that need to agree on it (the
// wrap itself, and the stat rows' rowY computed from however many lines the
// wrap used). 82 + kRouteLineHeight == 100, the fixed row-start y this card
// used before names existed, so a one-line route reproduces the old layout
// exactly rather than merely resembling it.
constexpr int kRouteLineHeight = 18;

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

// ---- The content budget -------------------------------------------------
//
// How far down a card is allowed to draw, which is NOT a constant: it depends
// on whether this card is about to get a button row painted over it.
//
// The bug this exists to fix. CardManager draws the card and THEN calls
// drawChrome(), which paints the buttons on top. The card was never told, so
// every card laid itself out against the full panel and a card with a button
// silently lost the 60px band from y=160 down - whatever it had drawn there was
// covered. On the home value card that band is where the "automated estimate,
// not an appraisal" line lands, and that line is not allowed to go missing.
//
// So the budget is set before the card draws, not after. A card that respects it
// uses contentBottom() as its floor and picks a tighter layout when the number
// comes back small; a card that ignores it is no worse off than before, and
// noteContentOverrun() below puts it in the debug stream so it stops being
// invisible.
//
// kClockTop is where drawClock() puts its bottom-right corner clock (roughly
// y 222-236), and nothing may overlap it in either mode.
constexpr int kClockTop = 220;
constexpr int kButtonBandGap = 6;

/// Set from CardManager before each card draws. Starts at the no-buttons value
/// so a card drawn outside the normal path (a boot splash, an error screen) sees
/// a sane budget rather than zero.
int gContentBottom = kClockTop;

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
  const String clockText = formatTimeOfDay(localTm.tm_hour, localTm.tm_min);

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
  // Raw rather than through layoutText(), and carrying the sentinel the CI
  // gate looks for. bottom_right is the datum this corner needs - the baseline
  // is pinned 4px off the bottom edge inside the 16px band described above,
  // which a top-anchored TextBox cannot express - and clockText is
  // formatTimeOfDay()'s own output, bounded to eight characters and already
  // clamped to "--:--" for anything it cannot render. There is no wire string
  // here for an ellipsis to protect.
  lcd.setFont(&fonts::FreeSansBold9pt7b);
  lcd.setTextSize(1);
  lcd.setTextColor(ink(), bg());
  lcd.setTextDatum(bottom_right);
  lcd.drawString(clockText, kScreenW - 6, kScreenH - 4);  // LAYOUT-PRIMITIVE
}

// ---- The one text layout routine ----------------------------------------
//
// Every string this file puts inside a bounded region goes through
// layoutText() below. There used to be five different ways of doing it and two
// of them were photographed failing on real hardware:
//
//   The sports card drew `Houston Astr`. drawTruncatedLeft() shortened the name
//   one character at a time until it measured narrow enough and then drew it
//   with nothing to say it had been shortened, so a household read a team whose
//   name appeared to be "Houston Astr".
//
//   The notice card drew `Test device: this rotation h` and then started the
//   next line at `one`, with the `as` of "has" nowhere on the panel. The old
//   greedy wrap recorded the space at i as a break point BEFORE measuring the
//   candidate ending at i, so when the overflow was discovered at a space the
//   break point was that same space: the line that had just been measured and
//   rejected was the line that got drawn, and the panel clipped the rest. The
//   measurement was correct and was then ignored.
//
// The second one is the reason this is one routine rather than four patched
// ones. Breaking at the last space STRICTLY BEFORE the overflowing word is a
// property of the routine now instead of a thing four call sites each have to
// get right. TEXT_LAYOUT_DESIGN.md at the repository root has the full survey,
// the guarantees, and the CI gate that stops a sixth approach appearing.
//
// The guarantees, because a caller should not have to read the body:
//
//   1. A line breaks at the last whitespace strictly before the word that
//      overflows, measured with lcd.textWidth() in whatever font the caller
//      selected. Never a character count, never an assumed glyph width.
//   2. Nothing is drawn wider than box.width. Every line is measured before it
//      goes out.
//   3. Nothing vanishes without a mark. Text that does not fit is wrapped, or
//      broken mid-word, or ellipsized with a visible "...". There is no fourth
//      outcome and there is no silent drop.
//   4. Nothing vanishes without a log line either. The remote debug stream is
//      the only diagnostic channel a wall-mounted panel has.
//   5. Nothing is allocated. Not one byte of heap on any path.
//
// Rule 5 is the one that constrains the implementation. mbedtls_ssl_setup()
// needs 16,717 CONTIGUOUS bytes twice (Http.h's kTlsRecordBufferBytes,
// DEVICE_MEMORY.md), and a device that cannot get them cannot check in, cannot
// report telemetry and cannot be told that the update fixing it exists. The
// helpers this replaces did the opposite: drawTruncatedLeft() took its String
// BY VALUE and then called substring() in a loop, so one long team name was a
// dozen heap allocations, on the draw path, every rotation. The line under
// construction here lives in a fixed stack buffer instead, and the String
// overload only ever calls c_str().

// kUseCardBackground is the sentinel meaning "use bg() for this text's own
// background colour", the ordinary case for every caller except
// showBannerCard() - a value no real 24-bit packed colour can equal, so it can
// share the `background` parameter rather than needing a separate bool flag.
constexpr uint32_t kUseCardBackground = 0xFFFFFFFFu;

enum class Align : uint8_t { Left, Centre, Right };

/// The region of panel a card is willing to spend on a string, and how many
/// lines of it. See TEXT_LAYOUT_DESIGN.md section 4 for what each field means
/// the card is promising.
struct TextBox {
  int16_t x;           // left edge for Left, centre for Centre, right edge for Right
  int16_t y;           // top of the first line
  int16_t width;       // pixels the text may occupy, measured, never assumed
  int16_t lineHeight;  // step from one line's top to the next
  uint8_t maxLines;    // the line budget, which has no default on purpose
  Align align;
};

struct TextResult {
  uint8_t lines = 0;      // lines drawn, or that would be drawn under measureOnly
  bool ellipsized = false;  // true when the "..." went on the panel
  int16_t bottom = 0;     // y just past the last line, for stacking the next block
};

// Above any line this panel can hold. The narrowest glyphs in the binary are
// Font0 at size 1, six pixels wide, so a 320px line is at most 53 characters;
// every GFX face the cards use is wider than that. A candidate that would
// exceed this is treated as a word too long to fit and logged loudly, because
// reaching it means a font assumption changed rather than that a string was
// long.
constexpr size_t kLayoutLineBytes = 128;

// Three ASCII periods, not U+2026. Every GFX font in this binary is ASCII only
// - this file already hand-draws a circle for the degree sign (see
// drawTemperature below) and the aircraft card writes "->" rather than an
// arrow glyph for the same reason. A single-glyph ellipsis would render as a
// box, which would be a worse lie than the truncation it exists to disclose.
constexpr char kEllipsis[] = "...";

/// Any byte at or below 0x20 is a break opportunity, not just ' '. A '\n' or
/// '\t' in an admin's announcement is otherwise handed to drawString() and
/// rendered as whatever box glyph this ASCII-only font has for it, in the
/// middle of a household's notice. Calendar.cpp already scrubs control
/// characters on the way in and says so; handling them here as well means the
/// next card to take free text does not have to remember.
bool isBreakSpace(char c) {
  return static_cast<unsigned char>(c) <= 0x20;
}

/// Copies text[from, to) into buffer and terminates it, turning any interior
/// control character into a space for the reason isBreakSpace() gives. Returns
/// false when the slice will not fit, which the caller treats as a word too
/// long rather than truncating behind its own back.
bool copySlice(char* buffer, size_t capacity, const char* text, size_t from, size_t to) {
  const size_t length = to - from;
  if (length + 1 > capacity) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    const char c = text[from + i];
    buffer[i] = isBreakSpace(c) ? ' ' : c;
  }
  buffer[length] = '\0';
  return true;
}

/// Draws text into box, wrapping on word boundaries and ellipsizing when the
/// line budget runs out. `what` names the string for the debug stream
/// ("sports.away", "notice.body") so a log line says which string on which card
/// lost characters - the same reasoning noteContentOverrun() takes a card name.
///
/// measureOnly runs the identical layout and returns the identical result
/// without putting anything on the panel, so a card can ask "would this
/// ellipsize at the size I have selected?" and choose a smaller one before
/// committing. Deliberately the same function rather than a parallel measuring
/// one, because a measurement that can drift from the drawing it predicts is
/// worse than no measurement at all.
TextResult layoutText(const char* text, const TextBox& box, uint32_t colour, const char* what,
                      uint32_t background = kUseCardBackground, bool measureOnly = false) {
  TextResult result;
  result.bottom = box.y;
  const char* who = (what == nullptr) ? "?" : what;

  if (box.maxLines == 0 || box.width <= 0) {
    // A layout mistake rather than a long string, so printf() rather than
    // verbose(): this one is worth seeing on Serial whether or not anybody
    // turned streaming on.
    Log::printf("[display] %s: box is %dpx wide with a %u line budget, so nothing was drawn",
                who, static_cast<int>(box.width), static_cast<unsigned>(box.maxLines));
    return result;
  }
  if (text == nullptr || text[0] == '\0') {
    // A card that silently drew nothing is a shape of bug this project has paid
    // for more than once, so the absence of ink gets a line in the stream the
    // same as the presence of it.
    if (!measureOnly) {
      Log::verbose("[display] %s: text is empty, nothing drawn", who);
    }
    return result;
  }

  lcd.setTextColor(colour, background == kUseCardBackground ? bg() : background);
  switch (box.align) {
    case Align::Centre: lcd.setTextDatum(top_center); break;
    case Align::Right:  lcd.setTextDatum(top_right);  break;
    case Align::Left:
    default:            lcd.setTextDatum(top_left);   break;
  }

  char line[kLayoutLineBytes];
  const size_t capacity = sizeof(line) - 1;
  const size_t length = strlen(text);
  size_t cursor = 0;
  size_t hardBreaks = 0;
  size_t dropped = 0;
  int drawY = box.y;

  while (cursor < length && result.lines < box.maxLines) {
    while (cursor < length && isBreakSpace(text[cursor])) {
      ++cursor;
    }
    if (cursor >= length) {
      break;
    }

    // How much of the remainder fits, word by word. lineEnd only ever advances
    // to a word boundary that has been MEASURED AND ACCEPTED, which is the
    // whole correction: the old code advanced it to the boundary that had just
    // been measured and rejected.
    size_t lineEnd = cursor;
    size_t scan = cursor;
    while (scan < length) {
      size_t wordEnd = scan;
      while (wordEnd < length && !isBreakSpace(text[wordEnd])) {
        ++wordEnd;
      }
      if (!copySlice(line, sizeof(line), text, cursor, wordEnd)) {
        break;
      }
      if (lcd.textWidth(line) > box.width) {
        break;
      }
      lineEnd = wordEnd;
      scan = wordEnd;
      while (scan < length && isBreakSpace(text[scan])) {
        ++scan;
      }
    }

    if (lineEnd == cursor) {
      // Not one word fits a line of its own, so this breaks mid-word and
      // carries the rest onto the next line. Not an ellipsis: a long URL under
      // a QR code is better as two ugly lines than as one line with its tail
      // deleted, and guarantee 3 says nothing is dropped while there is still
      // somewhere to put it. The ellipsis below is for running out of lines,
      // which is a different fact.
      size_t fitted = 0;
      for (size_t take = 1; cursor + take <= length && take <= capacity; ++take) {
        if (isBreakSpace(text[cursor + take - 1])) {
          break;
        }
        copySlice(line, sizeof(line), text, cursor, cursor + take);
        if (lcd.textWidth(line) > box.width) {
          break;
        }
        fitted = take;
      }
      if (fitted == 0) {
        // A single glyph wider than the whole box. The box is wrong, and
        // looping here forever would be worse than one overhanging character,
        // so this makes progress and says so loudly.
        Log::printf("[display] %s: a single glyph is wider than the %dpx box, drawing it anyway",
                    who, static_cast<int>(box.width));
        fitted = 1;
      }
      lineEnd = cursor + fitted;
      ++hardBreaks;
    }

    size_t rest = lineEnd;
    while (rest < length && isBreakSpace(text[rest])) {
      ++rest;
    }
    const bool moreToCome = rest < length;
    const bool lastLine = (static_cast<uint8_t>(result.lines + 1) >= box.maxLines);

    if (moreToCome && lastLine) {
      // Out of lines with text left over. Back characters off the end until the
      // line plus its "..." fits, so the ellipsis is inside the box rather than
      // hanging off the edge where the panel would clip the very mark that says
      // something was lost.
      size_t take = lineEnd - cursor;
      if (take > capacity - sizeof(kEllipsis)) {
        take = capacity - sizeof(kEllipsis);
      }
      while (true) {
        while (take > 0 && isBreakSpace(text[cursor + take - 1])) {
          --take;
        }
        copySlice(line, sizeof(line), text, cursor, cursor + take);
        strncat(line, kEllipsis, sizeof(line) - strlen(line) - 1);
        if (take == 0 || lcd.textWidth(line) <= box.width) {
          break;
        }
        --take;
      }
      dropped = length - (cursor + take);
      result.ellipsized = true;
      cursor = length;
    } else {
      copySlice(line, sizeof(line), text, cursor, lineEnd);
      cursor = lineEnd;
    }

    if (!measureOnly) {
      lcd.drawString(line, box.x, drawY);  // LAYOUT-PRIMITIVE
    }
    ++result.lines;
    drawY += box.lineHeight;
  }

  result.bottom = static_cast<int16_t>(drawY);

  // Left the datum where it found it, the way drawRightJustified() used to.
  // Half the card bodies below draw a literal label straight after a
  // right-aligned value, and a leaked top_right datum would silently
  // right-align the next thing somebody adds rather than failing visibly.
  lcd.setTextDatum(top_left);

  if (!measureOnly) {
    if (result.ellipsized) {
      Log::verbose("[display] %s: %u characters past line %u of %dpx did not fit, ellipsized",
                   who, static_cast<unsigned>(dropped), static_cast<unsigned>(box.maxLines),
                   static_cast<int>(box.width));
    }
    if (hardBreaks > 0) {
      Log::verbose("[display] %s: %u word(s) wider than the %dpx box were broken mid-word", who,
                   static_cast<unsigned>(hardBreaks), static_cast<int>(box.width));
    }
    if (result.lines == 0) {
      Log::printf("[display] %s: had %u characters and drew no lines at all", who,
                  static_cast<unsigned>(length));
    }
  }

  return result;
}

/// The same, for the const String& every public function in Display.h takes.
/// c_str() rather than a copy, so this overload allocates nothing either.
TextResult layoutText(const String& text, const TextBox& box, uint32_t colour, const char* what,
                      uint32_t background = kUseCardBackground, bool measureOnly = false) {
  return layoutText(text.c_str(), box, colour, what, background, measureOnly);
}

/// The single-line shape, which is most of this file: a stat value, a team
/// name, an address, a button label. One line means the "lines ran out" rule
/// applies immediately, so anything too wide ellipsizes rather than being
/// quietly shortened - which is exactly the sports card defect.
///
/// lineHeight comes from the font rather than from the caller because a
/// one-line box has no second line to step to, and a zero there would make the
/// returned `bottom` a quiet lie to anyone stacking a block underneath.
/// Both overloads exist for the same reason the layoutText() pair does: a
/// single const char* entry point taking a String would build a temporary on
/// every call, and half the call sites below hand it a snprintf'd char buffer
/// precisely to avoid that.
TextResult layoutLine(const char* text, int x, int y, int width, uint32_t colour,
                      const char* what, Align align = Align::Left,
                      uint32_t background = kUseCardBackground) {
  const TextBox box{static_cast<int16_t>(x), static_cast<int16_t>(y),
                    static_cast<int16_t>(width), static_cast<int16_t>(lcd.fontHeight()), 1,
                    align};
  return layoutText(text, box, colour, what, background);
}

TextResult layoutLine(const String& text, int x, int y, int width, uint32_t colour,
                      const char* what, Align align = Align::Left,
                      uint32_t background = kUseCardBackground) {
  return layoutLine(text.c_str(), x, y, width, colour, what, align, background);
}

/// How many lines of lineHeight actually fit between topY and the content
/// budget's floor. This is the join between this routine and the content budget
/// (README.md, "The content budget: a card knows whether a button will cover
/// it"): the budget says how far down a card may draw, and this turns that into
/// the maxLines the routine wants. A card whose text block is the last thing on
/// the panel should ask rather than write a constant, because a constant was
/// measured for exactly one of the two chrome configurations.
uint8_t linesToBudget(int topY, int lineHeight) {
  if (lineHeight <= 0) {
    return 0;
  }
  const int available = gContentBottom - topY;
  if (available < lineHeight) {
    return 0;
  }
  const int lines = available / lineHeight;
  return lines > 255 ? 255 : static_cast<uint8_t>(lines);
}

// The prose body the notice and calendar cards share: one short paragraph
// filling most of the panel, at the larger of two sizes whenever the whole
// thing survives it.
//
// One helper rather than the same eighteen lines in both cards, the same
// reasoning README.md's "One helper, not a conditional in every card" gives
// for the maintenance banner. Merging them also fixes a check that could not
// fail. Both copies asked for the line count at the large size WITH A FIVE
// LINE CAP and then tested `lines <= 5`, which a five line cap makes true
// every time, so the smaller fallback tier was unreachable code and a long
// notice was cut off at five lines with the denser size it was supposed to
// drop to never once being tried. What the card actually wants to know is
// whether the text survived, which is what TextResult::ellipsized answers and
// what a line count never could.
//
// The line budgets come from linesToBudget() rather than the constants 5 and
// 7. Those two numbers were measured against kButtonRowY when this card had no
// idea whether a button row was coming, and the 7x18px tier was recorded in
// README.md as landing 2px inside the button gap. Asking the budget gets the
// full panel when no button is bound and a correctly shorter block when one
// is, instead of one compromise that is slightly wrong in both cases.
constexpr int16_t kProseTopY = 30;
constexpr int16_t kProseLargeLineHeight = 24;
constexpr int16_t kProseSmallLineHeight = 18;

TextResult drawProseBody(const String& text, const char* what) {
  const int16_t bodyWidth = kScreenW - kCardMargin * 2;

  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextSize(1);
  const TextBox largeBox{kCardMargin, kProseTopY, bodyWidth, kProseLargeLineHeight,
                         linesToBudget(kProseTopY, kProseLargeLineHeight), Align::Left};
  if (!layoutText(text, largeBox, ink(), what, kUseCardBackground, /*measureOnly=*/true)
           .ellipsized) {
    return layoutText(text, largeBox, ink(), what);
  }

  lcd.setFont(&fonts::FreeSansBold9pt7b);
  const TextBox smallBox{kCardMargin, kProseTopY, bodyWidth, kProseSmallLineHeight,
                         linesToBudget(kProseTopY, kProseSmallLineHeight), Align::Left};
  Log::verbose("[display] %s: dropped to the 9pt tier, the 12pt one would have ellipsized", what);
  return layoutText(text, smallBox, ink(), what);
}

// Small colour-banded label in the top-left corner, e.g. CYD-Dickey's
// drawWeatherCard() doing `lcd.fillRect(0, 0, 110, 22, TFT_NAVY)` then
// printing "WEATHER" in bold white on top of it - every one of their cards
// (weather, listings, QR, branding) opens the same way, just with a
// different fixed width/colour/label. width is per-card because the label
// text itself varies ("WEATHER" vs "OVERHEAD").
//
// Every caller passes a literal today, and the label still goes through
// layoutText() rather than straight to drawString(). Nothing in the signature
// says a literal is required, and the one thing this file has learned is that
// "the caller will pass something short" is a promise the caller never made.
// The box is the coloured rect less the 8px inset on each side.
void drawCardBanner(const String& label, uint32_t bannerColour, int width) {
  lcd.fillRect(0, 0, width, kBannerHeight, bannerColour);
  lcd.setFont(&fonts::FreeSansBold9pt7b);
  lcd.setTextSize(1);  // GFX fonts are sized at their own point size - see drawCardBanner's callers
  const TextBox box{8, 4, static_cast<int16_t>(width - 16), kBannerHeight, 1, Align::Left};
  layoutText(label, box, kBannerLabelInk, "card.banner", bannerColour);
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

// The hand-drawn-degree-ring technique from the original centeredTemperature
// above, adapted to a left-aligned origin instead of screen-centered - this
// restyle's cards lay out left-margined (see layoutText's remarks), so
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

  // The three pieces below advance a cursor across the panel rather than
  // filling a box, so they do not go through layoutText() and carry the
  // sentinel the CI gate looks for instead. Both strings are bounded by
  // construction: a formatted int, and a unit this file's own callers pass as
  // "F" or "C". Nothing here comes off the wire.
  char numberText[12];
  snprintf(numberText, sizeof(numberText), "%d", temperature);
  int cursorX = x;
  lcd.drawString(numberText, cursorX, y);  // LAYOUT-PRIMITIVE
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

  lcd.drawString(unit, cursorX, y);  // LAYOUT-PRIMITIVE
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

// isDaytime only changes the Sunny case (a clear night is a moon, not a sun
// with rays reading as daylight it isn't) - every other condition already
// reads the same after dark as it does before it, so nothing else here
// branches on it.
void drawWeatherIcon(WeatherIconKind kind, int cx, int cy, int radius, bool isDaytime) {
  switch (kind) {
    case WeatherIconKind::Sunny:
      if (isDaytime) {
        drawSunIcon(cx, cy, radius);
      } else {
        drawMoonIcon(cx, cy, radius);
      }
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

uint8_t gBrightnessPercent = 100;

}  // namespace

void begin() {
  lcd.init();
  lcd.setRotation(1);  // 320x240 landscape
  lcd.setBrightness(255);
  clear();
}

/// Perceptual exponent for the backlight. 2.2 is the standard display gamma and
/// it is what makes a configured percentage mean what an operator expects.
///
/// WITHOUT THIS, THE NUMBERS LIE. A linear 25% PWM duty reads to the eye as
/// roughly half brightness, because human brightness perception is close to
/// logarithmic rather than linear. Observed directly on device 17 on 2026-09-16:
/// BacklightDimPercent was set to 25, the arithmetic was exactly right (duty
/// 63/255), and the operator's reaction was "I thought it would dim to 25% not
/// 75% on". The code was correct and the field was misleading, which is worse
/// than a bug because nothing looks wrong.
///
/// A second, quieter benefit: Motion's fade steps in PERCENT. Once percent is
/// perceptual, evenly-spaced percent steps are evenly-spaced to the eye as well,
/// so the fade stops crawling at the bottom of its range and jumping at the top
/// without needing any gamma logic of its own.
constexpr float kBacklightGamma = 2.2f;

void setBrightness(uint8_t percent) {
  if (percent > 100) {
    percent = 100;
  }
  gBrightnessPercent = percent;

  // 100% maps to 255 exactly rather than to whatever the curve rounds to, because
  // the pre-existing begin() sets 255 and a device that has never dimmed must look
  // identical to one that has dimmed and come back. 0 is likewise exact - the
  // backlight is genuinely off, not almost off.
  if (percent == 100) {
    lcd.setBrightness(255);
    return;
  }
  if (percent == 0) {
    lcd.setBrightness(0);
    return;
  }

  // powf rather than a lookup table: the ESP32 has hardware single-precision
  // floating point, this is called at most once per loop iteration during a fade,
  // and a 101-entry table would be the same numbers with somewhere else to drift.
  const float duty = powf(static_cast<float>(percent) / 100.0f, kBacklightGamma) * 255.0f;
  uint8_t level = static_cast<uint8_t>(duty + 0.5f);

  // A NON-ZERO PERCENT MUST LIGHT SOMETHING. The curve sends 1% to 255 * 0.0001,
  // which rounds to zero - and a panel asked for 1% that goes completely dark is
  // indistinguishable from a panel that was turned off, which is the exact
  // ambiguity that cost eight hours on this project the day before this was
  // written. Anything above zero gets at least one step of backlight.
  if (level == 0) {
    level = 1;
  }
  lcd.setBrightness(level);
}

uint8_t brightness() { return gBrightnessPercent; }

void setEnvironment(int utcOffsetMinutes, bool isDaytime) {
  gUtcOffsetMinutes = utcOffsetMinutes;
  gIsDaytime = isDaytime;
}

int utcOffsetMinutes() { return gUtcOffsetMinutes; }

void setUse12HourClock(bool use12Hour) { gUse12HourClock = use12Hour; }

bool use12HourClock() { return gUse12HourClock; }

String formatTimeOfDay(int hour24, int minute) {
  // Defensive rather than trusting: two of the eight call sites derive the hour
  // by dividing a minutes-of-day figure the server sent, so a bad value arrives
  // as arithmetic rather than as an obviously wrong struct tm. Clamping here
  // keeps a wrong time on screen from becoming a buffer full of nonsense.
  if (hour24 < 0 || hour24 > 23 || minute < 0 || minute > 59) {
    return String("--:--");
  }

  char buffer[9];
  if (!gUse12HourClock) {
    snprintf(buffer, sizeof(buffer), "%02d:%02d", hour24, minute);
    return String(buffer);
  }

  // Midnight and noon are the two the naive "% 12" gets wrong - both land on 0,
  // and "0:30 AM" is not a time anybody writes. 12-hour clocks have no zero.
  int hour12 = hour24 % 12;
  if (hour12 == 0) {
    hour12 = 12;
  }

  // No leading zero on the hour: "2:30 PM", not "02:30 PM". That is how a
  // 12-hour clock is written, and it also keeps the string one character
  // shorter, which matters in the right-justified stat columns on the tides and
  // sun/moon cards where this is 7 characters against 24-hour's 5.
  snprintf(buffer, sizeof(buffer), "%d:%02d %s", hour12, minute, hour24 < 12 ? "AM" : "PM");
  return String(buffer);
}

String describeAnswerAge(time_t observedAtUtc) {
  // Nothing on the wire. Absent, null, or a string this firmware's sscanf could
  // not read all arrive here as 0, and all three mean the same thing to a
  // reader: this card cannot say how old its answer is.
  if (observedAtUtc <= 0) {
    Log::verbose("[display] answer age: no observed-at timestamp, saying nothing");
    return String();
  }

  // THE CLOCK GATE. Everything below is `now - then`, and `now` is SNTP's
  // answer - which does not exist until AppService::synchroniseTime() has
  // landed, and does not exist at all on a device that booted without a
  // network. Before then time(nullptr) is a small number near the epoch and
  // the arithmetic below would confidently report tens of thousands of days.
  //
  // The threshold is "plainly later than this firmware was written" rather
  // than a precise sync flag, the same shape of check Calendar.cpp makes
  // before it computes "Today" and "Tomorrow": this module has no way to ask
  // whether SNTP succeeded, and a clock that is merely a few minutes out
  // changes nothing at this granularity.
  constexpr time_t kClockLooksSynchronised = 1735689600;  // 2025-01-01T00:00:00Z
  const time_t now = time(nullptr);
  if (now < kClockLooksSynchronised) {
    Log::verbose("[display] answer age: clock reads %ld, which is before 2025 - not synchronised "
                 "yet, so no age is claimed",
                 static_cast<long>(now));
    return String();
  }

  // A timestamp in the future is the server and this device disagreeing about
  // what time it is, not an answer from tomorrow. Saying nothing beats
  // rendering a negative age as an enormous unsigned one.
  if (observedAtUtc > now) {
    Log::verbose("[display] answer age: observed-at %ld is %ld s in the FUTURE of this device's "
                 "clock (%ld) - saying nothing rather than guessing",
                 static_cast<long>(observedAtUtc), static_cast<long>(observedAtUtc - now),
                 static_cast<long>(now));
    return String();
  }

  // See this function's declaration in Display.h for why the units step twice
  // and why the second step is at two days rather than one.
  const long ageSeconds = static_cast<long>(now - observedAtUtc);
  if (ageSeconds < 60) {
    return String("Updated just now");
  }
  const long ageMinutes = ageSeconds / 60;
  if (ageMinutes < 60) {
    // "1 min ago", never "1 mins ago" - the same singular the minute-only
    // helpers on the other cards already spell out by hand.
    return ageMinutes == 1 ? String("Updated 1 min ago")
                           : String("Updated ") + ageMinutes + " min ago";
  }
  const long ageHours = ageMinutes / 60;
  if (ageHours < 48) {
    return ageHours == 1 ? String("Updated 1 hour ago")
                         : String("Updated ") + ageHours + " hours ago";
  }
  // Truncating rather than rounding, deliberately, and it is the same choice
  // every branch above makes: a card may understate an answer's age by less
  // than one unit, and must never overstate it. "Updated 2 days ago" about
  // something 2 days and 20 hours old is a conservative claim; "3 days ago"
  // about something 2 days and 4 hours old is a wrong one.
  const long ageDays = ageHours / 24;
  return String("Updated ") + ageDays + " days ago";
}

bool readTouchRaw(int32_t& x, int32_t& y) {
  return lcd.getTouch(&x, &y);
}

// The boot-ladder screens. Both take server-supplied wording (a content-gate
// refusal, a provisioning message) with no length this file controls, which is
// why they wrapped before layoutText() existed and why the 8px margin either
// side is kept: kBootTextWidth is the old wrappedCenteredText's own
// kScreenW - 8 * 2.
constexpr int16_t kBootTextWidth = kScreenW - 16;

void showStatus(const String& headline, const String& detail) {
  clear();
  lcd.setTextSize(2);
  const TextBox headlineBox{kScreenW / 2, 85, kBootTextWidth, 22, 3, Align::Centre};
  const TextResult drew = layoutText(headline, headlineBox, ink(), "status.headline");
  if (detail.length() > 0) {
    lcd.setTextSize(1);
    const TextBox detailBox{kScreenW / 2, static_cast<int16_t>(drew.bottom + 12), kBootTextWidth,
                            14, 3, Align::Centre};
    layoutText(detail, detailBox, muted(), "status.detail");
  }
}

void showFailure(const String& headline, const String& whatToDo) {
  clear();
  lcd.setTextSize(2);
  const TextBox headlineBox{kScreenW / 2, 75, kBootTextWidth, 22, 3, Align::Centre};
  const TextResult drew = layoutText(headline, headlineBox, kWarn, "failure.headline");
  lcd.setTextSize(1);
  const TextBox detailBox{kScreenW / 2, static_cast<int16_t>(drew.bottom + 12), kBootTextWidth, 14,
                          3, Align::Centre};
  layoutText(whatToDo, detailBox, muted(), "failure.whattodo");
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
                      const String& originName, const String& destinationName,
                      const String& updatedAt) {
  lcd.fillScreen(bg());
  drawCardBanner("OVERHEAD", kAircraftBanner, 130);

  // Airline name is the headline when the server has one, in the spot
  // CYD-Dickey's drawFeaturedAircraft() gives the airline logo or bold
  // airline name - callsign was the fallback for this position for as long
  // as this server sent nothing richer (see Aircraft.h's updated remarks),
  // and stays the fallback now for a server too old to send a name at all.
  // Bounded at 200px rather than the full card width: aircraftLogoZone()
  // starts at x220, and a name long enough to reach it would run under the
  // logo rather than stopping short of it. An over-long name now ellipsizes
  // inside those 200px instead of being shortened with nothing to show for it.
  const bool hasAirlineName = airlineName.length() > 0;
  const String headline = hasAirlineName ? airlineName : callsign;
  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextSize(1);
  layoutLine(headline, kCardMargin, 32, 200, ink(), "aircraft.headline");

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
  layoutLine(distanceBuf, kCardMargin, 64, kScreenW - kCardMargin * 2, muted(),
             "aircraft.distance");

  // Route, in the gap between the distance line and the stat rows.
  // Name-with-code-fallback per side, independently - see Display.h's own
  // remarks on why an all-or-nothing switch would be wrong here (hexdb can
  // resolve one side's name and not the other's). Neither side present at
  // all draws no line at all - the honest rendering of "no route data", the
  // same reasoning Graphic.cpp draws nothing rather than an empty frame when
  // it has no picture configured.
  const String originDisplay = originName.length() > 0 ? originName : originCode;
  const String destinationDisplay = destinationName.length() > 0 ? destinationName : destinationCode;
  int routeLines = 0;
  if (originDisplay.length() > 0) {
    lcd.setFont(&fonts::FreeSansBold9pt7b);
    lcd.setTextSize(1);
    // "->" rather than a real arrow glyph, for the same reason as the
    // separator above - plain ASCII only.
    const String routeLine = destinationDisplay.length() > 0
        ? (originDisplay + " -> " + destinationDisplay)
        : ("from " + originDisplay);
    // A pure-code route ("KRDU -> KLGA") always fits kRouteLineHeight's
    // single line, the same one line this drew before names existed - the
    // wrap only ever engages for a name long enough to need it, which is why
    // a 6-month-old server's codes-only response reproduces this card's
    // original layout exactly rather than merely approximating it. Two lines
    // is the cap: a route that still doesn't fit in two now ellipsizes on the
    // second line rather than growing a third into the stat rows. No log line
    // here - this is the draw path, Aircraft.cpp's cardFetch() already logs
    // this exact same name-with-code-fallback route once per fetch rather than
    // once per draw, and layoutText() itself narrates an ellipsis.
    const TextBox routeBox{kCardMargin, 82, kScreenW - kCardMargin * 2, kRouteLineHeight, 2,
                           Align::Left};
    routeLines = layoutText(routeLine, routeBox, muted(), "aircraft.route").lines;
  }

  // Stat rows: a muted label on the left, the value right-aligned against the
  // card's right margin - the same label-left/value-right technique as
  // CYD-Dickey's drawFeaturedAircraft(), applied per-row here instead of to a
  // whole second column of airline-specific fields CAL didn't used to have
  // data for. The values are snprintf'd into a stack buffer rather than built
  // with String concatenation, which is a heap fix as much as a tidy-up: three
  // rows per draw was three String temporaries per draw on the heap whose
  // largest contiguous block decides whether TLS can open.
  //
  // rowY starts right after however many lines the route text actually
  // used, rather than a fixed y=100: a one-line (or absent) route reproduces
  // the fixed y=100 this had before names existed (82 + 1*18 == 100), and a
  // two-line name-based route pushes the rows down by exactly one more line
  // height instead of overlapping it. See Display.h's own remarks on why
  // this grows down rather than shrinking the font or truncating.
  const int rightX = kScreenW - kCardMargin;
  const int rowValueWidth = 150;
  // The label's own bound, which used to be nothing at all. The value column
  // is reserved first and the label gets what is left, so a label can never
  // push a value off the edge - the same ordering the sports card already
  // argues for its score column.
  const int rowLabelWidth = rightX - rowValueWidth - kCardMargin;
  int rowY = routeLines > 0 ? 82 + routeLines * kRouteLineHeight : 100;
  constexpr int kRowHeight = 30;

  char valueBuf[24];

  layoutLine("Altitude", kCardMargin, rowY, rowLabelWidth, muted(), "aircraft.altitude.label");
  snprintf(valueBuf, sizeof(valueBuf), "%d ft", altitudeFeet);
  layoutLine(valueBuf, rightX, rowY, rowValueWidth, ink(), "aircraft.altitude", Align::Right);
  rowY += kRowHeight;

  layoutLine("Speed", kCardMargin, rowY, rowLabelWidth, muted(), "aircraft.speed.label");
  snprintf(valueBuf, sizeof(valueBuf), "%d kts", static_cast<int>(speedKnots + 0.5));
  layoutLine(valueBuf, rightX, rowY, rowValueWidth, ink(), "aircraft.speed", Align::Right);
  rowY += kRowHeight;

  layoutLine("Heading", kCardMargin, rowY, rowLabelWidth, muted(), "aircraft.heading.label");
  layoutLine(compassDirection(headingDegrees), rightX, rowY, rowValueWidth, ink(),
             "aircraft.heading", Align::Right);
  rowY += kRowHeight;

  // Same defect Weather.cpp's restyle found and fixed on its own card: this
  // line was set in Font0, the 6x8 bitmap face drawClock()'s own remarks
  // record as having failed on real hardware ("simply not there"). Same
  // fix, same face.
  if (updatedAt.length() > 0) {
    lcd.setFont(&fonts::FreeSansBold9pt7b);
    lcd.setTextSize(1);
    layoutLine(updatedAt, kCardMargin, rowY + 4, kScreenW - kCardMargin * 2, muted(),
               "aircraft.updated");
  }

  drawClock();
  restoreDefaultFont();
}

// The three status cards below (aircraft, listings, forecast) share one shape:
// a headline in warn or muted, then an optional detail block stacked under
// however many lines the headline actually took. The `bottom` layoutText()
// returns is what does the stacking, so a two-line headline pushes the detail
// down by exactly one line instead of by an assumed one.
void showAircraftStatus(const String& headline, const String& detail, bool isProblem) {
  lcd.fillScreen(bg());
  drawCardBanner("OVERHEAD", kAircraftBanner, 130);

  lcd.setFont(&fonts::FreeSansBold9pt7b);
  lcd.setTextSize(1);
  const uint32_t headlineColour = isProblem ? kWarn : muted();
  const TextBox headlineBox{kCardMargin, 40, kScreenW - kCardMargin * 2, 22, 3, Align::Left};
  const TextResult drew = layoutText(headline, headlineBox, headlineColour, "aircraft.status");
  if (detail.length() > 0) {
    const TextBox detailBox{kCardMargin, static_cast<int16_t>(drew.bottom + 12),
                            kScreenW - kCardMargin * 2, 18, 3, Align::Left};
    layoutText(detail, detailBox, ink(), "aircraft.status.detail");
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
  // Stops at the icon column rather than at the value column: the icons below
  // are centred at x=130 and span 114-146, so a label allowed to run to x=160
  // would print through them.
  const int rowLabelWidth = 104;

  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextSize(1);

  layoutLine("Sunrise", kCardMargin, 44, rowLabelWidth, muted(), "sunmoon.sunrise.label");
  layoutLine(sunriseText, rightX, 44, rowValueWidth, ink(), "sunmoon.sunrise", Align::Right);

  layoutLine("Sunset", kCardMargin, 90, rowLabelWidth, muted(), "sunmoon.sunset.label");
  layoutLine(sunsetText, rightX, 90, rowValueWidth, ink(), "sunmoon.sunset", Align::Right);

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
    const TextBox detailBox{kCardMargin, 140, kScreenW - kCardMargin * 2, 20, 2, Align::Left};
    layoutText(detail, detailBox, muted(), "sunmoon.detail");
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

  // THE THREE COLUMNS, RE-MEASURED 2026-09-27 BECAUSE BOTH LABELS WERE BEING
  // EATEN. This card drew "Next..." on both rows on every device that had it -
  // tides.high.label and tides.low.label ellipsized on devices 12, 17 and 23,
  // which is every device with the card, and a photograph confirms it. The two
  // rows were then distinguishable only by their arrow icons, so the card no
  // longer said what it was for.
  //
  // The cause is visible in the comment this replaces: the label was squeezed
  // to 99px to clear an icon that had itself been moved right to clear the
  // label. At FreeSansBold12pt7b "Next high" measures 110px and "Next low"
  // 100px, so neither fitted - and because layoutText() wraps on word
  // boundaries before it ellipsizes, both fell back to the only word that fit
  // and drew "Next..." (71px). Off by eleven pixels on one row and by one on
  // the other, with the same useless result on both.
  //
  // The room came from the VALUE column, which had it. Every value this card
  // can draw is a clock time from Display::formatTimeOfDay(): 58px in 24-hour
  // form, at most 107px in 12-hour form ("11:11 AM", the widest of all 1,440
  // possibilities), or "--:--" at 38px. It was reserved 150. The columns now
  // measure:
  //
  //   label  x=10..132   (122px) - "Next high" is 110, so 12px spare
  //   icon   x=147..179  (32px, radius 16 centred at 163)
  //   value  x=194..310  (116px) - the widest possible time is 107, 9px spare
  //
  // with 15px of clear panel either side of the icon. Both rows keep identical
  // geometry, because a reader compares them.
  //
  // Labels are NOT shortened to "High"/"Low" to make them fit. "Next" is the
  // word that says these are upcoming times rather than the last ones, which
  // is the whole question somebody looks at this card to answer.
  const int rowValueWidth = 116;
  const int rowLabelWidth = 122;

  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextSize(1);

  layoutLine("Next high", kCardMargin, 44, rowLabelWidth, muted(), "tides.high.label");
  layoutLine(nextHighTideText, rightX, 44, rowValueWidth, ink(), "tides.high", Align::Right);

  layoutLine("Next low", kCardMargin, 90, rowLabelWidth, muted(), "tides.low.label");
  layoutLine(nextLowTideText, rightX, 90, rowValueWidth, ink(), "tides.low", Align::Right);

  // Same icon-in-the-gap placement as showSunMoonCard()'s two rows, and the
  // same radius 16 for the same reported reason (12 was too small to read at a
  // glance). It sits further right than showSunMoonCard()'s x=130 because
  // "Next high"/"Next low" are wider labels than "Sunrise"/"Sunset" - but the
  // centre is now derived from the two columns either side rather than nudged
  // by hand, which is what went wrong before: 32px centred in the gap between
  // the label's right edge (132) and the value column's left edge (194) puts
  // it at 163, spanning 147-179 with 15px clear on both sides.
  //
  // showSunMoonCard() above was checked for the same defect and does NOT have
  // it: "Sunrise" is 89px and "Sunset" 81px against its 104px label column, so
  // both fit with room to spare, which is why no sunmoon.*.label ellipsis ever
  // appeared in the telemetry. It is deliberately left alone.
  constexpr int kIconColumnX = 163;
  constexpr int kIconRadius = 16;
  drawTideIcon(kIconColumnX, 44 + 9, kIconRadius, /*rising=*/true);
  drawTideIcon(kIconColumnX, 90 + 9, kIconRadius, /*rising=*/false);

  drawClock();
  restoreDefaultFont();
}

void showHomeValueCard(const String& address, const String& estimateText, const String& rangeText,
                       const String& detail) {
  lcd.fillScreen(bg());
  drawCardBanner("HOME VALUE", kHomeValueBanner, 150);

  // The address leads, exactly as it does on showListingsCard() - a dollar
  // figure that names no house is the one thing on this card a reader cannot
  // check. Truncated rather than wrapped, and to ONE line: a formatted RentCast
  // address ("300 Eatons Landing Dr, Annapolis, MD 21401") runs to two lines at
  // 12pt and there is no vertical room for a second one, per the budget below.
  //
  // Absent is a real case - a valuation resolved from a GPS fix has no address
  // to print - and then this row is not drawn and everything below it moves
  // back up to where it was. An empty headline would leave a gap that reads as
  // a rendering fault rather than as an absent fact.
  const bool hasAddress = address.length() > 0;
  if (hasAddress) {
    lcd.setFont(&fonts::FreeSansBold12pt7b);
    lcd.setTextSize(1);
    layoutLine(address, kCardMargin, 30, kScreenW - kCardMargin * 2, ink(), "homevalue.address");
  }

  // THE VERTICAL BUDGET, and why the rows below tightened rather than simply
  // shifting down by the height of the new one. The screen is 240px tall. The
  // detail block can take two 18px lines and the compliance line another two,
  // so the old 44/90/140 layout already ran to roughly y=218. Adding a headline
  // on top of that without tightening would push the compliance line off the
  // bottom - and that is the line that is not allowed to go missing (see this
  // function's declaration in Display.h for why it is drawn here at all). The
  // gaps between the two stat rows were 46px and 50px for 17px-tall text, so
  // the room came out of those rather than off the end of the card.
  //
  // Tight mode is the same reasoning applied again, with less room. A button row
  // takes everything below y=154, and the compliance line still has to fit under
  // the detail block, so the address and both stat rows move up and close up.
  // The address is the first thing to go if even that is not enough - it is the
  // only element here that a reader can do without, since the card's own banner
  // already says what kind of thing this is.
  const bool tight = contentIsTight();
  const int firstRowY = tight ? (hasAddress ? 50 : 38) : (hasAddress ? 62 : 44);
  const int secondRowY = tight ? (hasAddress ? 80 : 72) : (hasAddress ? 100 : 90);

  // Same stat-row layout as showSunMoonCard()/showTidesCard() above: two
  // rows, label left and value right-justified. Unlike either of those,
  // "Range" arrives already rounded to the nearest thousand by HomeValue.cpp
  // rather than comma-grouped in full - see that module's own
  // formatThousands() remarks for why a fully-precise range does not fit
  // this column without either colliding with the label or being
  // character-truncated into a wrong number.
  const int rightX = kScreenW - kCardMargin;
  const int rowValueWidth = 150;
  const int rowLabelWidth = rightX - rowValueWidth - kCardMargin;

  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextSize(1);

  layoutLine("Est. value", kCardMargin, firstRowY, rowLabelWidth, muted(),
             "homevalue.estimate.label");
  layoutLine(estimateText, rightX, firstRowY, rowValueWidth, ink(), "homevalue.estimate",
             Align::Right);

  layoutLine("Range", kCardMargin, secondRowY, rowLabelWidth, muted(), "homevalue.range.label");
  layoutLine(rangeText, rightX, secondRowY, rowValueWidth, ink(), "homevalue.range", Align::Right);

  // detail (price-per-square-foot and the RentCast refresh date) is optional
  // and pushes the fixed compliance line below it down by however many lines
  // it actually used - the same "grow down rather than overlap" reasoning
  // showAircraftCard() applies to its own variable-height route line.
  int nextY = tight ? (hasAddress ? 108 : 100) : (hasAddress ? 138 : 140);

  // The compliance line is reserved FIRST, not fitted last. It is the one line
  // on this card that is not allowed to go missing, so the detail block gets
  // whatever is left above it rather than the other way round - and when that is
  // nothing, the detail is dropped. Price per square foot and a refresh date are
  // worth having; they are not worth pushing a legal qualifier off the panel,
  // which is exactly what happened when a button appeared on this card.
  const int complianceHeight = 18;
  const int complianceY = contentBottom() - complianceHeight;

  if (detail.length() > 0) {
    const int roomForDetail = complianceY - 6 - nextY;
    const int detailLinesAllowed = roomForDetail / 18;
    if (detailLinesAllowed >= 1) {
      lcd.setFont(&fonts::FreeSansBold9pt7b);
      const TextBox detailBox{
          kCardMargin, static_cast<int16_t>(nextY), kScreenW - kCardMargin * 2, 18,
          static_cast<uint8_t>(detailLinesAllowed > 2 ? 2 : detailLinesAllowed), Align::Left};
      nextY = layoutText(detail, detailBox, muted(), "homevalue.detail").bottom + 6;
    } else {
      Log::verbose("[display] homevalue dropped its detail line - %d px left above the "
                   "compliance line at y=%d",
                   roomForDetail, complianceY);
    }
  }

  // The one line this function draws unconditionally, regardless of what any
  // of the four parameters say - see this function's own declaration in
  // Display.h for why this compliance wording lives here rather than in
  // whatever HomeValue.cpp happened to pass as `detail`. Drawn at the reserved
  // position, or lower if the detail block ended up below it, so it is never
  // overlapped by the thing above it either.
  const int drawComplianceAt = nextY > complianceY ? nextY : complianceY;
  lcd.setFont(&fonts::FreeSansBold9pt7b);
  layoutLine("Automated estimate, not an appraisal.", kCardMargin, drawComplianceAt,
             kScreenW - kCardMargin * 2, muted(), "homevalue.compliance");
  noteContentOverrun("homevalue", drawComplianceAt + complianceHeight);

  drawClock();
  restoreDefaultFont();
}

void showSportsCard(const String& homeName, const String& homeScore, const String& awayName,
                    const String& awayScore, const String& status, uint16_t itemNumber,
                    uint16_t itemCount, const String& ageText) {
  lcd.fillScreen(bg());
  drawCardBanner("SPORTS", kSportsBanner, 110);

  // Two team rows, each a name on the left and a score hard right. The score
  // column is reserved first, so a long club name can never push a score off
  // the edge or onto a second line - the score is the thing somebody crossing
  // the room is trying to read.
  constexpr int kScoreColumnWidth = 64;
  constexpr int kHomeRowY = 62;
  constexpr int kAwayRowY = 116;
  const int nameWidth = kScreenW - kCardMargin * 2 - kScoreColumnWidth;
  const int rightX = kScreenW - kCardMargin;

  lcd.setTextSize(1);

  for (int row = 0; row < 2; ++row) {
    const bool isAway = (row == 1);
    const String& name = isAway ? awayName : homeName;
    const String& score = isAway ? awayScore : homeScore;
    const int y = isAway ? kAwayRowY : kHomeRowY;

    // THE AWAY ROW CARRIES AN "@". Two stacked names say nothing about which
    // team is at home, and "@ Houston Astros" is the convention every American
    // scoreboard and ticker uses, so a reader crossing the room resolves it
    // without thinking. Only the away row is marked: a home team with no
    // marker is the home team, and marking both would spend width saying what
    // the absence already says.
    //
    // The "@" is joined to the name BEFORE anything is measured, which is the
    // part that matters here. Added after the fit was computed it would push
    // the name one glyph further into being cut, and the marker itself could
    // end up being the thing the ellipsis ate - the layout would have created
    // the defect it exists to fix.
    //
    // 96 bytes against a wire cap of 20 characters (Sports.h's
    // kMaxTeamNameLength). That is not a guess at what fits the panel, which
    // is roughly 14 glyphs at this font; it is room for anything a caller
    // could hand this function, so the join never loses a character behind
    // layoutText's back. A caller that somehow exceeds it says so out loud.
    char nameLine[96];
    const char* nameToDraw = name.c_str();
    if (isAway && name.length() > 0) {
      const int needed = snprintf(nameLine, sizeof(nameLine), "@ %s", name.c_str());
      if (needed < 0 || static_cast<size_t>(needed) >= sizeof(nameLine)) {
        Log::printf("[display] sports.away: name is %u characters, longer than the %u byte "
                    "join buffer, so the '@' row was cut before it was measured",
                    static_cast<unsigned>(name.length()), static_cast<unsigned>(sizeof(nameLine)));
      }
      nameToDraw = nameLine;
    }

    // Two tiers, the same technique showAnnouncementCard() uses for its body
    // text: try the size the card was designed at, and drop one tier when the
    // whole name will not survive it. "Houston Astros" is 14 characters and
    // does not fit 236px at 18pt, which is how it reached a photograph reading
    // "Houston Astr"; it fits comfortably at 12pt. An ellipsis is still there
    // as the backstop for a name that will not fit either size, so this is a
    // way of needing the ellipsis less often rather than a way of avoiding it.
    //
    // The score stays at 18pt regardless. This card's own reasoning is that
    // the score is what a reader across the room is after, so the name is the
    // one that gives up size.
    lcd.setFont(&fonts::FreeSansBold18pt7b);
    const int largeHeight = lcd.fontHeight();
    const TextBox largeBox{kCardMargin, static_cast<int16_t>(y), static_cast<int16_t>(nameWidth),
                           static_cast<int16_t>(largeHeight), 1, Align::Left};
    const bool wouldCut =
        layoutText(nameToDraw, largeBox, ink(), "sports.name", kUseCardBackground,
                   /*measureOnly=*/true)
            .ellipsized;

    if (wouldCut) {
      lcd.setFont(&fonts::FreeSansBold12pt7b);
      // Centred against the 18pt score beside it rather than sharing its top
      // edge, which would leave the smaller name floating high in the row.
      const int nudge = (largeHeight - lcd.fontHeight()) / 2;
      layoutLine(nameToDraw, kCardMargin, y + nudge, nameWidth, ink(),
                 isAway ? "sports.away" : "sports.home");
      lcd.setFont(&fonts::FreeSansBold18pt7b);
    } else {
      layoutText(nameToDraw, largeBox, ink(), isAway ? "sports.away" : "sports.home");
    }

    // Empty before play starts, and that is drawn as nothing rather than as a
    // zero. Null and zero are different facts here: a nil-nil draw is a real
    // scoreline and a game that has not started is not 0-0.
    if (score.length() > 0) {
      layoutLine(score, rightX, y, kScoreColumnWidth, ink(),
                 isAway ? "sports.awayscore" : "sports.homescore", Align::Right);
    }
  }

  // The status row: a clock time before, the provider's own progress text
  // during, FINAL or PPD after. Empty when the server sent a state this
  // firmware does not know, and then this row is simply absent - claiming a
  // game has not started is exactly the claim an unknown state cannot support.
  //
  // Bounded short of the "N of M" counter that shares this row, rather than
  // across the whole card: the counter is drawn after and would otherwise be
  // printed over by a long enough progress string.
  // THERE IS NO FOURTH ROW ON THIS CARD, AND THAT IS THE POINT.
  //
  // The obvious home for the age line - a line of its own at y=198 - measures
  // as free and is not. setContentBudget() drops the content floor to
  // kButtonRowY - kButtonBandGap = 154 whenever this card has an action bound,
  // and drawChrome() then paints the button row over y=160..220. This card is
  // also one of the thirteen that never consults contentBottom() and never
  // calls noteContentOverrun(), so anything placed down there would be covered
  // silently with nothing in the stream to say so. The status and counter rows
  // below y=160 are ALREADY being painted over on a device with a button
  // bound: "nothing is drawn at y=205" is therefore not evidence that the room
  // is free, it is evidence that the region belongs to the chrome.
  //
  // So the age goes on the row that already exists, in the slot the counter
  // already occupies, and this function draws no pixel lower than it did
  // before. Whatever is wrong with this card's relationship to the button row
  // stays exactly as wrong as it was rather than becoming one element worse.
  constexpr int kMarkerColumnWidth = 64;
  constexpr int kStatusRowY = 172;
  constexpr int kStatusBoxWidth = kScreenW - kCardMargin * 2 - kMarkerColumnWidth;  // 236
  // The 9pt right-hand slot sits 4px lower than the 12pt status so an 18px
  // line is optically centred against the 23px one beside it. That is the
  // counter's existing offset, kept because the age takes the counter's place.
  constexpr int kRightSlotY = 176;
  constexpr int kStatusToAgeGap = 10;

  // MEASURED, so the age gets the room the status actually leaves rather than
  // the room a worst case would leave. The status runs from "T7" (28px) through
  // a start time ("11:11 AM", 107px) to an eight-character provider period
  // ("HALFTIME", 124px), and reserving for the widest would deny the age a
  // place on every ordinary card. Clamped to the box, because layoutLine()
  // ellipsizes anything longer down to it and the ink on the panel is then
  // never wider than this.
  lcd.setFont(&fonts::FreeSansBold12pt7b);
  int statusInkWidth = 0;
  if (status.length() > 0) {
    statusInkWidth = lcd.textWidth(status.c_str());
    if (statusInkWidth > kStatusBoxWidth) {
      statusInkWidth = kStatusBoxWidth;
    }
    layoutLine(status, kCardMargin, kStatusRowY, kStatusBoxWidth, muted(), "sports.status");
  }

  // HOW OLD THIS ANSWER IS - empty, and this whole block skipped, on every card
  // the server has not flagged as old, which is nearly all of them. See this
  // function's declaration in Display.h and CARD_ABSENCE_AND_AGE_DESIGN.md
  // section 8 for why it appears rarely rather than always.
  //
  // THE AGE NEVER ELLIPSIZES. It is a qualifier on the card's content and not
  // the content, so when the status word has not left room for the whole
  // sentence this draws nothing and says so in the stream. "Updated 47 hou..."
  // would be worse than no line at all: the card would have spent its one spare
  // slot on something that no longer states an age, and a reader would have no
  // way to tell what was lost. The status keeps its natural width either way -
  // it is the thing somebody crossed the room to read.
  bool drewAge = false;
  if (ageText.length() > 0) {
    lcd.setFont(&fonts::FreeSansBold9pt7b);
    const int roomForAge = kScreenW - kCardMargin - kCardMargin - statusInkWidth -
                           (statusInkWidth > 0 ? kStatusToAgeGap : 0);
    const int ageInkWidth = lcd.textWidth(ageText.c_str());
    if (ageInkWidth <= roomForAge) {
      layoutLine(ageText, rightX, kRightSlotY, roomForAge, muted(), "sports.age", Align::Right);
      drewAge = true;
    } else {
      // printf rather than verbose: this is the card declining to say something
      // it was asked to say, on one of the few draws where it had anything to
      // say at all, and noticing it must not depend on somebody having switched
      // streaming on first.
      Log::printf("[display] sports.age: '%s' needs %dpx and the status word left %dpx, so this "
                  "card draws no age rather than an ellipsized one",
                  ageText.c_str(), ageInkWidth, roomForAge);
    }
  }

  // "2 of 4", only on a card actually holding several games, so a one-game team
  // card is not decorated with a counter that never changes.
  //
  // YIELDS TO THE AGE, because they are one 64px slot and both cannot have it.
  // On the rare card old enough for the server to say so, "this answer is
  // thirty-one hours old" is worth more than "you are looking at the second of
  // four": the counter says where you are in a list the rotation will show you
  // anyway, the age says whether any of it is still true. Announced rather than
  // silent, because a household used to seeing a counter will notice it gone.
  if (itemCount > 1 && !drewAge) {
    char marker[16];
    snprintf(marker, sizeof(marker), "%u of %u", static_cast<unsigned>(itemNumber),
             static_cast<unsigned>(itemCount));
    lcd.setFont(&fonts::FreeSansBold9pt7b);
    layoutLine(marker, rightX, kRightSlotY, kMarkerColumnWidth, muted(), "sports.counter",
               Align::Right);
  } else if (itemCount > 1) {
    Log::printf("[display] sports.counter: '%u of %u' dropped this draw so the age line can have "
                "its slot - this card is stale, and saying so is worth more than the item number",
                static_cast<unsigned>(itemNumber), static_cast<unsigned>(itemCount));
  }

  // LEFT AS FOUND, and it does nothing. CardManager::drawCurrent() calls
  // setContentBudget() immediately BEFORE card.draw(), so this assignment lands
  // after the only read that could have mattered and is overwritten before the
  // next one. Noted rather than removed because this card lays itself out
  // against fixed pixel rows and never consults the budget at all - the status
  // row at 172 is already inside a bound button's row - so tidying this single
  // line would imply a relationship to the budget that this card does not have.
  // Reported separately: the fix is a layout change to the whole card.
  gContentBottom = 200;
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
  const int rowLabelWidth = rightX - rowValueWidth - kCardMargin;

  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextSize(1);

  layoutLine("Distance", kCardMargin, 44, rowLabelWidth, muted(), "iss.distance.label");
  layoutLine(distanceText, rightX, 44, rowValueWidth, ink(), "iss.distance", Align::Right);

  layoutLine("Direction", kCardMargin, 90, rowLabelWidth, muted(), "iss.direction.label");
  layoutLine(directionText, rightX, 90, rowValueWidth, ink(), "iss.direction", Align::Right);

  if (detail.length() > 0) {
    lcd.setFont(&fonts::FreeSansBold9pt7b);
    const TextBox detailBox{kCardMargin, 140, kScreenW - kCardMargin * 2, 20, 2, Align::Left};
    layoutText(detail, detailBox, muted(), "iss.detail");
  }

  drawClock();
  restoreDefaultFont();
}

void showIssNextPassCard(const String& riseTimeText, const String& riseDirectionText,
                          const String& detail) {
  lcd.fillScreen(bg());
  drawCardBanner("ISS", kIssFlyoverBanner, 90);

  // Same stat-row layout and same banner/icon as showIssFlyoverCard()
  // immediately above - this is that card's other display mode, not a
  // different card (see IssFlyover.h) - just with "Next pass"/"Direction"
  // rows for a future rise instead of "Distance"/"Direction" for a live
  // position, and a detail line about the peak of the pass instead of the
  // live coordinates.
  const int rightX = kScreenW - kCardMargin;
  const int rowValueWidth = 150;
  const int rowLabelWidth = rightX - rowValueWidth - kCardMargin;

  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextSize(1);

  layoutLine("Next pass", kCardMargin, 44, rowLabelWidth, muted(), "iss.nextpass.label");
  layoutLine(riseTimeText, rightX, 44, rowValueWidth, ink(), "iss.nextpass", Align::Right);

  layoutLine("Direction", kCardMargin, 90, rowLabelWidth, muted(), "iss.risedir.label");
  layoutLine(riseDirectionText, rightX, 90, rowValueWidth, ink(), "iss.risedir", Align::Right);

  if (detail.length() > 0) {
    lcd.setFont(&fonts::FreeSansBold9pt7b);
    const TextBox detailBox{kCardMargin, 140, kScreenW - kCardMargin * 2, 20, 2, Align::Left};
    layoutText(detail, detailBox, muted(), "iss.detail");
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
    layoutLine(phaseName, kScreenW / 2, 150, kBootTextWidth, ink(), "moon.phase", Align::Centre);
  }

  char pctBuffer[24];
  snprintf(pctBuffer, sizeof(pctBuffer), "%d%% illuminated", static_cast<int>(k * 100.0 + 0.5));
  lcd.setFont(&fonts::FreeSansBold9pt7b);
  // The buffer straight through rather than String(pctBuffer): the temporary
  // that wrapping cost was a heap allocation on the draw path for a string
  // that is already a flat array.
  layoutLine(pctBuffer, kScreenW / 2, 176, kBootTextWidth, muted(), "moon.illuminated",
             Align::Centre);

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
  // One of the three raw draws left in this file, and the only one on a card.
  // The hero wants middle_center so the numerals sit centred on y=100, which
  // is a datum a top-anchored TextBox does not express, and it already
  // measures and gives up a whole size rather than clipping - the one thing
  // layoutText() would add is an ellipsis on a string that formatTimeOfDay()
  // bounds to eight characters. If even size 1 will not fit, that is a font
  // change nobody accounted for, so it says so rather than quietly clipping.
  if (lcd.textWidth(timeText) > maxTimeWidth) {
    Log::printf("[display] clockdate: '%s' is %dpx wide at the smallest size, past the %dpx card",
                timeText.c_str(), lcd.textWidth(timeText), maxTimeWidth);
  }
  lcd.drawString(timeText, kScreenW / 2, 100);  // LAYOUT-PRIMITIVE

  // The date, secondary to the time both in size and in colour (muted(),
  // same as every other card's supporting line) - the same bold 9pt/12pt
  // family the rest of this file uses rather than a plain bitmap face, and
  // wrapped rather than assuming a spelled-out weekday and month always fits
  // on one line at this width.
  lcd.setFont(&fonts::FreeSansBold12pt7b);
  const TextBox dateBox{kScreenW / 2, 145, kBootTextWidth, 20, 2, Align::Centre};
  layoutText(dateText, dateBox, muted(), "clockdate.date");

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
void showCalendarCard(const String& text, uint8_t itemNumber, uint8_t itemCount) {
  lcd.fillScreen(bg());
  drawCardBanner("CALENDAR", kCalendarBanner, 118);

  // "2 of 3", drawn beside the banner rather than under the text. Calendar is a
  // list card whose items cycle one at a time, so without this a household
  // looking at a single appointment cannot tell whether it is the only one or
  // the first of several - and the difference between "nothing else today" and
  // "something else is coming" is most of what a glance at this card is for.
  // Omitted for a lone item, where "1 of 1" is noise.
  if (itemCount > 1 && itemNumber >= 1) {
    lcd.setFont(&fonts::FreeSansBold9pt7b);
    lcd.setTextSize(1);
    // snprintf into a buffer rather than String(itemNumber) + " of " + ... :
    // that expression was three heap allocations per draw for a string this
    // card already knows the exact shape of.
    char counter[16];
    snprintf(counter, sizeof(counter), "%u of %u", static_cast<unsigned>(itemNumber),
             static_cast<unsigned>(itemCount));
    layoutLine(counter, kScreenW - kCardMargin, 8, 64, muted(), "calendar.counter", Align::Right);
  }

  // The body reuses the announcement card's two-tier wrap verbatim rather than
  // inventing its own: the sizes were chosen against this exact screen and
  // button row, and an event line is the same shape of content as a notice -
  // one short paragraph of prose. What was wrong before was never the layout,
  // only the green "NOTICE" banner stamped above it.
  if (text.length() > 0) {
    drawProseBody(text, "calendar.body");
  }

  drawClock();
  restoreDefaultFont();
}

void showAnnouncementCard(const String& text) {
  lcd.fillScreen(bg());
  drawCardBanner("NOTICE", kAnnouncementBanner, 90);

  // Starts at y=30 rather than the original y=40 - kButtonRowY moved from 190
  // to 160 when the button row doubled in height, and shifting this block's
  // own start up by 10px buys back exactly the clearance that move cost. How
  // many lines fit under that start is now the content budget's answer rather
  // than this card's guess: see drawProseBody().
  //
  // This is the card whose first line was photographed reading "Test device:
  // this rotation h" with the rest of "has" clipped off the panel. That was
  // the wrap breaking at the space it had just measured and rejected, and it
  // is fixed in layoutText() rather than here, because the listings card and
  // the QR caption were making the identical mistake through the identical
  // function.
  if (text.length() > 0) {
    drawProseBody(text, "notice.body");
  }

  drawClock();
  restoreDefaultFont();
}

// The Banner / Banner Button themes - see Display.h's own remarks and
// Cards::Theme. Deliberately not built on drawCardBanner()/showAnnouncementCard():
// this is a genuinely different layout shape (a strip the full width of the
// panel, tall enough to hold the whole message) rather than a corner label
// plus a full card body, so it earns its own function instead of a parameter
// bolted onto either of those.
//
// Below the strip is left as plain bg() - no second block of content, no
// second banner - which is the deliberate visual difference from Full Screen
// this theme exists to make: the empty space between the strip and the
// button row/clock is the signal that this is a partial-screen reminder, not
// a card that was replaced.
void showBannerCard(const String& text) {
  lcd.fillScreen(bg());
  lcd.fillRect(0, 0, kScreenW, kBannerStripHeight, kBannerStripFill);

  if (text.length() > 0) {
    lcd.setFont(&fonts::FreeSansBold12pt7b);
    // Centred, not left-margined like showAnnouncementCard()'s body text -
    // a short reminder read from across a room sits better centred in a
    // strip than ranged left against an edge nothing else in the strip lines
    // up with. Three lines at 26px (78px) inside a 100px-tall strip leaves
    // an 11px margin top and bottom.
    const TextBox stripBox{kScreenW / 2, 11, kScreenW - kCardMargin * 2, 26, 3, Align::Centre};
    layoutText(text, stripBox, kBannerStripInk, "banner.strip", kBannerStripFill);
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
    layoutLine("Cannot display code", kCardMargin, 60, bodyWidth, ink(), "qr.failure");
    if (qrData.length() > 0) {
      // The payload itself, which is the one thing on this card a reader could
      // still act on. Word-broken mid-token when it has to be, because a URL
      // has no spaces and half of one is no use to anybody.
      const TextBox dataBox{kCardMargin, 110, bodyWidth, 18, 3, Align::Left};
      layoutText(qrData, dataBox, muted(), "qr.failure.data");
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
    // The 22px step is this card's own rather than the font's advance: its
    // vertical budget is measured to the pixel (see the header comment above)
    // and FreeSansBold12pt7b's line advance is several pixels taller than the
    // spacing this layout was built around.
    layoutLine(caption, kScreenW / 2, y, bodyWidth, ink(), "qr.caption", Align::Centre);
    y += 22 + 2;
  }
  lcd.setFont(&fonts::FreeSansBold9pt7b);
  // One line, not two, once a caption is already on screen - see this
  // function's own header comment for why.
  const TextBox dataBox{kScreenW / 2, static_cast<int16_t>(y), static_cast<int16_t>(bodyWidth),
                        16, static_cast<uint8_t>(hasCaption ? 1 : 2), Align::Centre};
  layoutText(qrData, dataBox, muted(), "qr.data");

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
  // 64px is the counter's own column in the banner row, to the right of the
  // coloured rect that ends at x=130. It does not constrain the address below,
  // which is a row further down; it bounds the counter itself, which used to be
  // drawn right-aligned against the margin with nothing saying how far left it
  // was allowed to grow.
  constexpr int kCounterWidth = 64;
  if (total > 1) {
    lcd.setFont(&fonts::FreeSansBold9pt7b);
    lcd.setTextSize(1);
    char caption[16];
    snprintf(caption, sizeof(caption), "%u of %u", static_cast<unsigned>(index) + 1,
             static_cast<unsigned>(total));
    layoutLine(caption, kScreenW - kCardMargin, 4, kCounterWidth, muted(), "listings.counter",
               Align::Right);
  }

  // Headline: the address itself, the one fact that actually identifies
  // *this* listing from the last one shown. A formatted RentCast address runs
  // long, so this is the card's most likely ellipsis and it is now an
  // ellipsis rather than a character count silently thrown away.
  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextSize(1);
  layoutLine(address, kCardMargin, 32, kScreenW - kCardMargin * 2, ink(), "listings.address");

  // Price and property type share the sub-headline - the same "two related
  // facts, one line" pairing showAircraftCard() gives callsign+distance.
  lcd.setFont(&fonts::FreeSansBold9pt7b);
  String subline = formatPrice(price);
  if (propertyType.length() > 0) {
    subline += " - " + propertyType;
  }
  layoutLine(subline, kCardMargin, 64, kScreenW - kCardMargin * 2, muted(), "listings.subline");

  // Stat rows: identical label-left/value-right technique to
  // showAircraftCard()'s Altitude/Speed/Heading block - the same shape of
  // information, a house instead of a plane.
  const int rightX = kScreenW - kCardMargin;
  const int rowValueWidth = 150;
  const int rowLabelWidth = rightX - rowValueWidth - kCardMargin;
  int rowY = 88;
  constexpr int kRowHeight = 26;

  layoutLine("Beds", kCardMargin, rowY, rowLabelWidth, muted(), "listings.beds.label");
  layoutLine(formatCount(bedrooms), rightX, rowY, rowValueWidth, ink(), "listings.beds",
             Align::Right);
  rowY += kRowHeight;

  layoutLine("Baths", kCardMargin, rowY, rowLabelWidth, muted(), "listings.baths.label");
  layoutLine(formatCount(bathrooms), rightX, rowY, rowValueWidth, ink(), "listings.baths",
             Align::Right);
  rowY += kRowHeight;

  layoutLine("Sq Ft", kCardMargin, rowY, rowLabelWidth, muted(), "listings.sqft.label");
  char sqftBuf[16];
  if (squareFootage > 0) {
    snprintf(sqftBuf, sizeof(sqftBuf), "%d", squareFootage);
  } else {
    // A dash, not a zero. An unknown floor area and a zero-square-foot house
    // are different claims, and only one of them is one this card can make.
    snprintf(sqftBuf, sizeof(sqftBuf), "-");
  }
  layoutLine(sqftBuf, rightX, rowY, rowValueWidth, ink(), "listings.sqft", Align::Right);
  rowY += kRowHeight;

  layoutLine("Listed", kCardMargin, rowY, rowLabelWidth, muted(), "listings.listed.label");
  layoutLine(formatDaysOnMarket(daysOnMarket), rightX, rowY, rowValueWidth, ink(),
             "listings.listed", Align::Right);
  rowY += kRowHeight;

  // Footer: distance and freshness together, both pinned to a fixed baseline
  // below the stat rows rather than flowing under them - same reasoning
  // showWeatherCard()'s own freshness line gives for not letting this move
  // around the card as other fields change length.
  char distanceBuf[32];
  snprintf(distanceBuf, sizeof(distanceBuf), "%.1f mi away", distanceMiles);
  lcd.setFont(&fonts::FreeSansBold9pt7b);
  lcd.setTextSize(1);
  // The freshness column is reserved first, so the distance gets what is left
  // of the row rather than the two of them overprinting each other.
  //
  // 190px, MEASURED, and it was 160 - which was already too narrow for the
  // wording this card had before today. At FreeSansBold9pt7b "Updated just
  // now" is 150px and fits, but "Updated 1 min ago" is 161 and "Updated 59 min
  // ago" is 171, so every value except the shortest one has been ellipsizing
  // to "Updated 1..." since the column was written. The 24-hour cache fix
  // (Listings.cpp, CARD_ABSENCE_AND_AGE_DESIGN.md section 2a) makes the
  // longest forms routine rather than rare - "Updated 47 hours ago" is 189 and
  // "Updated 365 days ago" is 190 - so the column is sized to the longest
  // string Display::describeAnswerAge() can produce within a year.
  //
  // The 110px that leaves for the distance is measured too: "99.9 mi away" is
  // 109px, so everything inside a plausible search radius fits. A three-digit
  // distance ("999.9 mi away", 119px) ellipsizes, which is the right way round
  // - a listing a thousand miles away is not what this card is for, and an
  // ellipsis says something was dropped rather than hiding it.
  //
  // The two columns abut with no gap at their simultaneous worst case. That is
  // deliberate rather than overlooked: 300px of row cannot hold both worst
  // cases and a margin, the realistic pair ("3.2 mi away" and "Updated 5 hours
  // ago") leaves about 22px of clear space between them, and a tight row beats
  // either string losing its tail.
  constexpr int kFreshnessWidth = 190;
  layoutLine(distanceBuf, kCardMargin, rowY + 4, kScreenW - kCardMargin * 2 - kFreshnessWidth,
             muted(), "listings.distance");
  if (updatedAt.length() > 0) {
    layoutLine(updatedAt, rightX, rowY + 4, kFreshnessWidth, muted(), "listings.updated",
               Align::Right);
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
  const TextBox headlineBox{kCardMargin, 40, kScreenW - kCardMargin * 2, 22, 3, Align::Left};
  const TextResult drew = layoutText(headline, headlineBox, headlineColour, "listings.status");
  if (detail.length() > 0) {
    const TextBox detailBox{kCardMargin, static_cast<int16_t>(drew.bottom + 12),
                            kScreenW - kCardMargin * 2, 18, 3, Align::Left};
    layoutText(detail, detailBox, ink(), "listings.status.detail");
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
//
// Three characters is the wording wanted ("Mon"), not a width fallback, which
// is why this still counts characters rather than measuring. The measuring
// happens where it belongs, at the draw: the label goes through layoutText()
// against the real column width, so a font change that makes three characters
// too wide for 60px ellipsizes and says so instead of printing over the
// neighbouring column.
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
  constexpr int kDayNightWidth = 64;
  layoutLine(currentIsDaytime ? "Day" : "Night", kScreenW - kCardMargin, 4, kDayNightWidth,
             muted(), "forecast.daynight", Align::Right);

  // Location, same placement as the retired showWeatherCard()'s own line -
  // this card answers the identical "72 degrees *where*" question that one
  // did.
  if (location.length() > 0) {
    // Stops short of the Day/Night tag above it rather than running the full
    // card width: the tag is drawn first and a long enough city name would
    // otherwise print straight through it.
    layoutLine(location, kCardMargin, 28, bodyWidth - kDayNightWidth, muted(),
               "forecast.location");
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
                  kHeroIconRadius, currentIsDaytime);

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
    layoutLine(currentShortForecast, 76, 80, kScreenW - 76 - kCardMargin, ink(),
               "forecast.condition");
  }

  if (updatedAt.length() > 0) {
    lcd.setFont(&fonts::FreeSansBold9pt7b);
    lcd.setTextSize(1);
    layoutLine(updatedAt, kCardMargin, 104, bodyWidth, muted(), "forecast.updated");
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
    layoutLine(shortDayLabel(i, dayNames[i]), columnCentreX, 122, kColumnWidth, muted(),
               "forecast.daylabel", Align::Centre);

    // Always the daytime variant: each strip column summarises a whole day,
    // not a specific night, so there is no isDaytime of its own to read the
    // way the hero above reads currentIsDaytime for "right now".
    drawWeatherIcon(classifyCondition(dayConditions[i]), columnCentreX, 155, /*radius=*/17,
                    /*isDaytime=*/true);

    char tempBuffer[12];
    snprintf(tempBuffer, sizeof(tempBuffer), "%d%s", dayTemperatures[i], dayUnits[i].c_str());
    layoutLine(tempBuffer, columnCentreX, 178, kColumnWidth, ink(), "forecast.daytemp",
               Align::Centre);
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
  const TextBox headlineBox{kCardMargin, 40, kScreenW - kCardMargin * 2, 22, 3, Align::Left};
  const TextResult drew = layoutText(headline, headlineBox, headlineColour, "forecast.status");
  if (detail.length() > 0) {
    const TextBox detailBox{kCardMargin, static_cast<int16_t>(drew.bottom + 12),
                            kScreenW - kCardMargin * 2, 18, 3, Align::Left};
    layoutText(detail, detailBox, ink(), "forecast.status.detail");
  }

  drawClock();
  restoreDefaultFont();
}

void showNoContent(const String& headline, const String& detail) {
  lcd.fillScreen(bg());

  lcd.setFont(&fonts::FreeSansBold9pt7b);
  lcd.setTextSize(1);
  const TextBox headlineBox{kCardMargin, 60, kScreenW - kCardMargin * 2, 22, 3, Align::Left};
  const TextResult drew = layoutText(headline, headlineBox, muted(), "nocontent.headline");
  if (detail.length() > 0) {
    const TextBox detailBox{kCardMargin, static_cast<int16_t>(drew.bottom + 12),
                            kScreenW - kCardMargin * 2, 18, 3, Align::Left};
    layoutText(detail, detailBox, muted(), "nocontent.detail");
  }

  drawClock();
  restoreDefaultFont();
}

void setContentBudget(bool hasActionButtons) {
  gContentBottom = hasActionButtons ? kButtonRowY - kButtonBandGap : kClockTop;
}

int contentBottom() { return gContentBottom; }

bool contentIsTight() { return gContentBottom < kClockTop; }

void noteContentOverrun(const char* cardName, int reachedY) {
  if (reachedY <= gContentBottom) {
    return;
  }
  // Logged rather than clipped. Clipping would hide the problem behind a card
  // that merely looks a bit short; the stream is the only diagnostic channel a
  // deployed device has, and a card overrunning its budget is exactly the kind
  // of thing nobody would otherwise notice until a household complained that a
  // line they needed was missing.
  Log::printf("[display] card '%s' drew to y=%d, past its %d budget - %d px of it is under %s",
              cardName == nullptr ? "?" : cardName, reachedY, gContentBottom,
              reachedY - gContentBottom, contentIsTight() ? "the button row" : "the clock");
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

  // This used to carry its own copy of the truncate-a-character-at-a-time
  // loop, which is how a server label like "Request a brochure" reached the
  // glass as "Request a broch" with nothing saying so. The label is the
  // server's wording drawn verbatim and it has no idea how wide this panel
  // is, so it goes through the same routine every card uses.
  //
  // Two lines rather than one. A 60px button has room for them at this font,
  // and a two-line label is a label a household can read where an ellipsized
  // one is a guess. The ellipsis is still the backstop for a label too long
  // for even two lines.
  const int maxTextWidth = w - 10;
  const int lineHeight = lcd.fontHeight();
  const TextBox measuring{static_cast<int16_t>(x + w / 2), 0, static_cast<int16_t>(maxTextWidth),
                          static_cast<int16_t>(lineHeight), 2, Align::Centre};
  const uint8_t lines =
      layoutText(label, measuring, kButtonInk, "button.label", fill, /*measureOnly=*/true).lines;

  // Centred in the button's own height, which is what middle_center used to
  // do for a single line and what a fixed top y would get wrong the moment a
  // label wrapped onto a second one.
  const int blockHeight = (lines > 0 ? lines : 1) * lineHeight;
  TextBox drawing = measuring;
  drawing.y = static_cast<int16_t>(y + h / 2 - blockHeight / 2);
  layoutText(label, drawing, kButtonInk, "button.label", fill);
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

/// How many card draws in a row have failed, for App.ino's heap watchdog.
///
/// **This counts observed harm, not a heap metric, and that distinction is the
/// point.** Every earlier version of that watchdog compared a heap figure
/// against a threshold, and every one of those thresholds turned out to be
/// indefensible: 60,000 and then 28,000 were both derived from
/// ESP.getMaxAllocHeap(), which reports a value pinned at exactly 32,756 on
/// every device and every boot while the real largest 8BIT block was measured
/// at 6,132. A device restarted on that basis every four minutes for a
/// fragmentation it could never clear.
///
/// "This device could not draw its card" needs no theory about which pool is
/// short and cannot be fooled by a metric that means something other than it
/// appears to. Reset to zero by any successful draw.
///
/// Read through consecutiveDrawFailures() rather than exported directly, so
/// nothing outside this file can write it: the count is only meaningful if
/// exactly one place decides what counts as a failure.
///
/// **What this counts, stated narrowly, because a wider reading of it rebooted
/// a household's device every eleven minutes for an evening.** It counts draws
/// where an image was actually in front of the decoder and the decoder would
/// not produce a picture from it. It does NOT count a draw that never reached a
/// decoder because there was no image to give one - see noteNoImageAvailable()
/// immediately below for why that distinction is load-bearing rather than
/// pedantic, and what it cost to learn.
uint32_t gConsecutiveDrawFailures = 0;

void noteDrawOutcome(bool ok) {
  if (ok) {
    gConsecutiveDrawFailures = 0;
  } else {
    ++gConsecutiveDrawFailures;
  }
}

/// A draw that never got as far as a decode, because there was no image in
/// front of it: the file would not open, or the RAM buffer handed over was
/// empty. Logs, and deliberately touches the counter in neither direction.
///
/// **Not counted, because it is not evidence of harm.** "There is no picture
/// here" is the ordinary resting state of an unconfigured card, of a card whose
/// asset has not been fetched yet, and of every graphic card on a device with
/// no SD card at all. A device sitting in that state is not failing; it is
/// doing the quiet thing Graphic.h says a picture card should do when it has no
/// picture. Feeding it to a watchdog whose remedy is "restart to reclaim
/// memory" asks the device to solve a shortage it does not have.
///
/// **Not reset either, which is the half that is easy to get wrong.** A missing
/// image says nothing about whether this device can draw, so it is no more
/// evidence of recovery than it is of harm. Clearing a run of genuine decode
/// failures because a different card happened to have nothing to show would let
/// one silent card mask another card's real, repeated inability to render.
/// Neither counted nor cleared: simply not evidence.
///
/// `what` names the thing that was not there (a path, or a description of the
/// buffer) so the log line still identifies the card, and `why` says which of
/// the two cases this was.
void noteNoImageAvailable(const char* why, const String& what) {
  Log::printf("[display] nothing to draw: %s (%s) - not counted as a draw failure", why,
              what.c_str());
}

/// The heap by capability class, for comparing across a specific moment.
///
/// **Both the free size and the largest block, per class.** A large free total
/// with a small largest block is fragmentation; a small free total is genuine
/// exhaustion; and the two differing BETWEEN classes would mean memory that is
/// free but stranded in regions no byte-addressable allocation can use.
/// MALLOC_CAP_32BIT is included only for that last comparison - a large 32BIT
/// block beside a small 8BIT one would say the "free" memory was never
/// available to malloc at all.
///
/// Measured once already, and the answer was none of the exotic cases: at a
/// failing 10,568-byte allocation, all four classes reported an identical
/// 11,340 free with a 6,132 largest block and integrity OK. No DMA starvation,
/// no stranding, no corruption - just less room than the ESP wrappers claimed.
void logHeapSnapshot(const char* when) {
  Log::printf(
      "[heapdiag] %s | 8BIT free=%u largest=%u | INT|8BIT free=%u largest=%u | "
      "DMA free=%u largest=%u | 32BIT largest=%u | ESP.maxAlloc=%u",
      when, static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
      static_cast<unsigned>(
          heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_32BIT)),
      static_cast<unsigned>(ESP.getMaxAllocHeap()));
}

/// Which decoder a cached file needs, decided by reading the file's own first
/// bytes rather than by being told.
///
/// **Nothing on the wire carries a format, and nothing should.** The obvious
/// alternative - a `format` field beside the asset id in the check-in response
/// - fails on this project's own compatibility rule: the server keeps working
/// with device firmware up to six months old, so a field only new firmware
/// reads is a field the server cannot depend on, and a field old firmware
/// ignores cannot change what an old device does with the bytes it already
/// has. It would also be answering the wrong question. What matters at draw
/// time is not "what does the catalog say this asset is" but "what is in the
/// file on this card, right now" - and during any rollout those two diverge,
/// because /assets holds whatever each entry happened to be when it was
/// fetched. One device's cache will carry PNGs pulled last month beside JPEGs
/// pulled this morning, and re-fetching everything to make the cache uniform
/// is precisely the network traffic the cache exists to avoid.
///
/// A self-describing file needs no wire field, no new device state, and no
/// agreement between a server version and a firmware version. It is also the
/// only scheme whose answer cannot be stale, because the thing being asked is
/// the thing about to be decoded.
/// `Missing` is NOT a format and is not a decode outcome - it is "there was no
/// file to look at". It was folded into `Unknown` until an evening spent
/// watching a card-less device reboot itself, and separating the two is the
/// whole of this file's half of that fix.
///
/// sniffImageFormat() below has always said, in its own comment, that a file
/// that would not open is "distinct from 'opened but unrecognised' on purpose"
/// - and then returned the same enumerator for both, so every caller
/// immediately lost the distinction it had just been told mattered. The two
/// really are different findings about different subsystems: unrecognised bytes
/// mean a file arrived and is not a picture (a cached HTML error page, a
/// truncated write - a real failure this device should be counted against),
/// while a file that will not open means there is no cached picture here at all
/// (no card, a card pulled at runtime, a filesystem that did not mount, or an
/// asset that invalidate() removed between the caller's SD.exists() check and
/// this draw). Only the first is a draw this device failed.
///
/// `Rgb565` is the one format in this list that is NOT a standard file type
/// somebody else's tool would recognise: it is a raw frame buffer in this
/// panel's own pixel layout behind a 16-byte header this project defined (see
/// kRgb565Signature and drawRgb565FromSd() below). A raw bitmap has no magic
/// number of its own, and that is a genuine design problem rather than a
/// detail - the whole scheme above rests on a file being able to identify
/// itself. Three options were on the table and only one keeps that property:
///
///   - A FILE EXTENSION. Rejected for the reasons Assets.cpp's kExtension
///     comment already gives at length: the cache filename is a KEY, nothing
///     reads its extension, and making it truthful turns one SD.exists() into
///     a search. It would also mis-identify every file already on a card.
///   - A MANIFEST or card-policy FIELD. Rejected for the reason at the top of
///     this comment: it answers the wrong question. What matters at draw time
///     is what is in this file on this card right now, not what the catalog
///     believes, and during any rollout those diverge.
///   - A CONTAINER with its own magic. Chosen. Sixteen bytes of header make
///     the file self-describing exactly as a PNG or JPEG is, so nothing about
///     this format travels on the wire, nothing has to be agreed between a
///     server version and a firmware version, and the answer cannot be stale.
///     It also carries the dimensions, which bare pixels cannot: 153,600
///     bytes is 320x240 and equally 240x320, and a device guessing would draw
///     a diagonal smear.
///
/// Worth recording that a breaking wire change WAS available and was not
/// needed. DeploymentStage.IsProduction is false on the server as of
/// 2026-09-11 and the 6-month firmware compatibility rule is suspended, so a
/// format field could simply have been added; the container is better on its
/// own merits, and the rule returns the moment anything ships.
enum class ImageFormat { Png, Jpeg, Rgb565, Unknown, Missing };

const char* formatName(ImageFormat format) {
  switch (format) {
    case ImageFormat::Png:
      return "png";
    case ImageFormat::Jpeg:
      return "jpeg";
    case ImageFormat::Rgb565:
      return "rgb565";
    case ImageFormat::Missing:
      return "no such file";
    default:
      return "unknown";
  }
}

// The three signatures this firmware recognises. JPEG's is the SOI marker
// followed by the first byte of the next marker (FF D8 FF); PNG's is the full
// eight-byte signature from the specification; RGB565's is the ASCII "DAM5"
// this project chose for its own container - printable on purpose, so a person
// with the card in a reader can identify one with any hex viewer.
constexpr uint8_t kJpegSignature[] = {0xFF, 0xD8, 0xFF};
constexpr uint8_t kPngSignature[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
constexpr uint8_t kRgb565Signature[] = {'D', 'A', 'M', '5'};
constexpr size_t kHeaderBytes = sizeof(kPngSignature);

// --- The RGB565 container, byte for byte. Must match Rgb565Image.cs on the
//     server, which is the writer; these two are the only readers/writers of
//     this format anywhere and each one names the other.
//
//   0..3   magic "DAM5"
//   4      version, kRgb565Version
//   5      flags; bit 0 set means little-endian pixels (this firmware
//          declines those - see below)
//   6..7   width,  big-endian uint16
//   8..9   height, big-endian uint16
//   10..15 reserved, MUST be ignored rather than required to be zero, or the
//          first use of one of them breaks every older build
constexpr size_t kRgb565HeaderBytes = 16;
constexpr uint8_t kRgb565Version = 1;
constexpr uint8_t kRgb565FlagLittleEndianPixels = 0x01;
constexpr size_t kRgb565BytesPerPixel = 2;

/// How many scanlines are read and pushed at a time, and the one number in
/// this whole path worth arguing about.
///
///   8 lines x 320 px x 2 bytes = 5,120 bytes, in ONE contiguous malloc.
///
/// **Why 8 and not more.** The buffer has to be contiguous, and this file's
/// own measurements are the constraint: at draw time the largest free 8BIT
/// block on this hardware has been observed as low as 5,876 bytes, and a
/// 10,568-byte request failed outright against a 6,132-byte block. 5,120 fits
/// under even the 5,876 low-water mark, with 756 bytes to spare. Sixteen lines
/// would be 10,240 - within a rounding error of the request that was actually
/// failing in the field, which is not a coincidence to design against.
///
/// **Why not fewer.** There is no memory argument for 4 lines (2,560 bytes)
/// and a small cost: each band is one SD read and one pushImage, and 5,120
/// bytes is exactly 10 FAT sectors, so 8 lines keeps both counts low without
/// approaching the ceiling. 30 bands draw a full 320x240 frame.
///
/// **The comparison that matters.** LovyanGFX's JPEG decoder takes a
/// 3,900-byte workspace; its PNG decoder wants roughly 45,056 and keeps it.
/// So this path's peak allocation is the same order as the cheap decoder's and
/// an order below the expensive one's - and unlike either, it does not vary
/// with the picture. A decoder's appetite depends on the image; a band of
/// scanlines does not.
constexpr size_t kRgb565BandLines = 8;

/// The hard ceiling on one band buffer, so an unexpectedly wide image reduces
/// its band count rather than asking for a block this heap does not have.
/// Equal to the 8-line figure above at the panel's own 320px width, which is
/// the only width the server currently produces - this exists for the case
/// where that stops being true, and it is why the loop below computes its band
/// height from the width instead of trusting kRgb565BandLines outright.
constexpr size_t kRgb565MaxBandBytes = kRgb565BandLines * 320 * kRgb565BytesPerPixel;

/// Classifies the bytes at the front of an image, wherever they came from.
///
/// Split out from sniffImageFormat() below so the SD path and the
/// straight-from-RAM path cannot drift apart on what counts as a JPEG. Two
/// copies of a magic-byte test is exactly the kind of duplication that stays
/// correct until one of them is updated.
///
/// Tests the MAGIC ONLY for RGB565, deliberately: version, flags and length
/// are the draw path's business (see drawRgb565FromSd()). A file with a future
/// version number therefore identifies as this format and is declined with an
/// explanation naming the version, rather than being reported as unrecognised
/// bytes - which is the difference between "this build is too old for that
/// picture" and "your SD card is corrupt", two findings that send a person
/// looking in completely different places.
ImageFormat classifyHeader(const uint8_t* header, size_t length) {
  if (header == nullptr) {
    return ImageFormat::Unknown;
  }
  if (length >= sizeof(kRgb565Signature)) {
    bool raw = true;
    for (size_t i = 0; i < sizeof(kRgb565Signature); ++i) {
      if (header[i] != kRgb565Signature[i]) {
        raw = false;
        break;
      }
    }
    if (raw) {
      return ImageFormat::Rgb565;
    }
  }
  if (length >= sizeof(kJpegSignature)) {
    bool jpeg = true;
    for (size_t i = 0; i < sizeof(kJpegSignature); ++i) {
      if (header[i] != kJpegSignature[i]) {
        jpeg = false;
        break;
      }
    }
    if (jpeg) {
      return ImageFormat::Jpeg;
    }
  }
  if (length >= sizeof(kPngSignature)) {
    bool png = true;
    for (size_t i = 0; i < sizeof(kPngSignature); ++i) {
      if (header[i] != kPngSignature[i]) {
        png = false;
        break;
      }
    }
    if (png) {
      return ImageFormat::Png;
    }
  }
  return ImageFormat::Unknown;
}

/// Reads the first eight bytes of `path`, and nothing else.
///
/// Eight rather than the four that would settle PNG-vs-JPEG on their own,
/// because the read costs the same either way - the card hands up a whole
/// sector to answer any of this - and the four bytes past "\x89PNG" are the
/// ones the PNG specification put there deliberately: 0D 0A 1A 0A is a
/// tripwire for a transport that mangled CR/LF or stopped at an EOF byte.
/// These files arrive over HTTPS and are written to a FAT card by this
/// firmware, which is exactly the class of path that can damage a file while
/// leaving its first four bytes perfectly intact, so keeping the cheap half of
/// the signature and discarding the diagnostic half would be giving up the
/// only part that says something we could not already guess.
///
/// Opened and closed here, rather than handing a live handle to the decode.
/// The decoder opens the same path again a moment later, which is one extra
/// directory lookup and worth it: keeping a handle across the dispatch would
/// mean either threading it through two different LovyanGFX overloads or
/// remembering to close it on every early return, and this file has already
/// paid for forgetting a release path once (see releaseDecodeMemory()).
ImageFormat sniffImageFormat(const String& path) {
  File file = SD.open(path.c_str());
  if (!file) {
    // Distinct from "opened but unrecognised" on purpose: this is a missing
    // card, a missing file or a filesystem that did not mount, none of which
    // are a question about image formats.
    //
    // That sentence stood here while this line returned Unknown, which handed
    // every caller the exact opposite of what it says. ImageFormat::Missing is
    // the enumerator that makes the distinction survive the return - see the
    // enum's own remarks.
    Log::printf("[display] cannot open %s to identify it", path.c_str());
    return ImageFormat::Missing;
  }
  uint8_t header[kHeaderBytes] = {0};
  const size_t got = file.read(header, sizeof(header));
  file.close();

  const ImageFormat format = classifyHeader(header, got);
  if (format == ImageFormat::Unknown) {
    // All eight bytes go in the line, because they identify the cause where a
    // bare "unrecognised" would not: a cached HTML error page starts 3C 21
    // ("<!"), a truncated or zero-length write shows up as a short read, and a
    // PNG whose 0D 0A 1A 0A tail was mangled in transport has its first four
    // bytes right and its tail wrong. Those are three different bugs in three
    // different subsystems, and this is the line that tells them apart -
    // printing only the leading four would put the reader straight back to
    // guessing. Bytes past a short read read as 00, because header[] was
    // zeroed before the read.
    //
    // Hand-rolled rather than snprintf("%02X ") because a fixed-width nibble
    // table cannot be made to truncate or to disagree with its format string,
    // and this runs on the diagnostic path of a device whose only diagnostic
    // channel is the log line itself.
    static const char kHexDigits[] = "0123456789ABCDEF";
    char hex[kHeaderBytes * 3] = {0};
    for (size_t i = 0; i < kHeaderBytes; ++i) {
      hex[i * 3] = kHexDigits[(header[i] >> 4) & 0x0F];
      hex[i * 3 + 1] = kHexDigits[header[i] & 0x0F];
      hex[i * 3 + 2] = ' ';
    }
    // Replaces the trailing space with the terminator, so the buffer is exactly
    // "89 50 4E 47 0D 0A 1A 0A" and no wider than it needs to be.
    hex[sizeof(hex) - 1] = '\0';
    Log::printf("[display] %s has no recognised image signature (read %u bytes, header %s)",
                path.c_str(), static_cast<unsigned>(got), hex);
  } else if (format != ImageFormat::Missing) {
    // Says what the file IS, next to the name that suggests something else.
    //
    // **This line exists because the log was reading as a contradiction.**
    // Observed on device 17:
    //
    //   [display] drew 320x240 rgb565 from /assets/89003ca7-....png in 30
    //             bands of 5120 bytes (no decoder)
    //
    // That file holds a DAM5 container of raw 16-bit pixels and is not a PNG,
    // and the same is true of every JPEG on the card. The naming is correct and
    // deliberate - Assets.cpp's kExtension sets out at length why the filename
    // is a cache KEY and not a type declaration, and why making it truthful
    // would turn one SD.exists() into a directory search on a device whose
    // scarce resources are contiguous memory and code nobody has to reason
    // about twice. Nothing about that changes here. What changes is that the
    // log stops letting the path imply a format and states the one actually
    // detected from the file's own first bytes.
    //
    // Placed at this choke point rather than in each draw path, which is the
    // cheapest correct place: every SD draw in this file sniffs before it draws
    // (drawImageFromSd(), drawImageFromSdInRect()), so one line here covers
    // PNG, JPEG and RGB565 at once, covers a format added later for free, and
    // does not need three per-decoder lines kept in agreement. The rgb565 draw
    // line further down still prints the path, and now reads correctly because
    // this line has already said what the bytes are.
    //
    // Log::verbose, not printf, for the same reason the rgb565 draw line is
    // verbose: this is the boring path, it runs on every card that has a
    // picture, and a line per draw emitted unconditionally would crowd out the
    // failures worth reading. The contradiction this fixes is only visible to
    // someone reading a stream in the first place.
    Log::verbose("[display] %s is %s - the .png suffix is a cache key, not the format "
                 "(see Assets.cpp's kExtension); the format here was read from the file's own "
                 "header",
                 path.c_str(), formatName(format));
  }
  return format;
}

/// Draws a raw RGB565 file, a band of scanlines at a time, with no decoder in
/// the picture at all.
///
/// **What this path costs, which is the entire reason it exists.** One
/// contiguous malloc of kRgb565BandBytes (5,120 at the panel's width - see
/// kRgb565BandLines for that arithmetic and why 8 lines rather than 16 or 4),
/// freed on every exit path. That figure does not depend on the image: a busy
/// photograph and a flat colour cost exactly the same, where a decoder's
/// workspace and its failure modes both vary with content. Compare what this
/// file has measured of the alternatives - 3,900 bytes for the JPEG decoder,
/// roughly 45,056 retained for the PNG one, against a largest free 8BIT block
/// seen as low as 5,876 at draw time.
///
/// **The decode already happened, on the server, once.** Every
/// decoder-shaped failure in this file - consecutiveDrawFailures(),
/// releaseDecodeMemory()'s whole argument, the evening device 7 spent
/// rebooting itself because a card-less boot never gets a roomy moment to
/// allocate a pngle - is a cost of doing image decoding on a machine with
/// ~90KB of contiguous heap on a good day. This format moves that work to a
/// machine with gigabytes. What is left here is a file read and a memcpy.
///
/// **The price, stated plainly:** the file is roughly ten times a JPEG of the
/// same picture, and there is no way to make it smaller. On SD that is free -
/// device 17 is using 160KB of a 7.81GB card - and on a device with NO card it
/// is fatal, because those fetch an asset into one contiguous heap buffer and
/// 153,600 bytes contiguous does not exist on this hardware. The server's card
/// policy editor refuses the assignment for a device that has reported having
/// no card, which is where that has to be caught: by the time the bytes are in
/// front of this function there is nothing useful left to do about it.
///
/// **Byte order, and how it was settled rather than guessed.** The file holds
/// big-endian RGB565 - high byte first, red in the top five bits of the first
/// byte - which is what the ILI9341 wants on the wire and what LovyanGFX calls
/// swap565_t. That was read out of the library source rather than assumed,
/// because a wrong byte order here produces a recognisable image in wrong
/// colours, which reads as a subtle rendering bug rather than as a format
/// error and is the classic way this goes wrong.
///
/// **CONFIRMED ON HARDWARE, 2026-09-12.** Device 17 drew a 320x240 panel-native
/// asset beside the identical picture as a JPEG - a control card added for
/// exactly this comparison - and the two matched. That is the only thing that
/// can settle byte order here: the container has no checksum over pixel
/// content, the draw reports success either way, and every automated test
/// asserts the bytes the ENCODER emits rather than what the panel makes of
/// them. Until somebody looked at the screen this was reasoned and unverified.
///
/// The reasoning is kept below, because it is what to re-check if a future core
/// changes these pixel types underneath:
///
///   - lgfx/v1/misc/colortype.hpp:263 declares swap565_t as bitfields
///     gh:3, r5:5, b5:5, gl:3 in a uint16_t. Little-endian allocation puts
///     raw = (gl<<13)|(b5<<8)|(r5<<3)|gh, so the two bytes in memory are
///     [RRRRRGGG][GGGBBBBB] - high byte of the RGB565 value FIRST.
///   - Line 45 of the same header confirms it from the other direction:
///     swap565(r,g,b) puts red-plus-green-high in the LOW byte of raw, which
///     is the first byte in memory.
///   - misc/pixelcopy.hpp:108 shows a destination depth of rgb565_2Byte -
///     what a 16-bit panel writes - IS swap565_t, and pixelcopy.cpp:41 sets
///     no_convert when source and destination depths match. So a swap565_t*
///     push is a straight copy with no per-pixel work.
///
///   The mirror-image type, rgb565_t (colortype.hpp:120, b5:5 g6:6 r5:5,
///   depth rgb565_nonswapped), is the LITTLE-endian layout and is what a plain
///   uint16_t* would be taken for. Pushing big-endian bytes as rgb565_t - or
///   the reverse - is exactly the mistake described above.
///
/// The container carries a flag bit for the byte order anyway, and this
/// function honours it: a little-endian file is pushed as rgb565_t and takes
/// LovyanGFX's per-pixel conversion path. The server never writes one, so that
/// branch is unexercised; it is here because a file that SAYS which order it
/// is in is worth more than a convention two codebases have to remember, and
/// because the alternative to handling it is drawing the wrong colours.
///
/// Centred inside the given box using the panel's clip rect rather than by
/// arithmetic on the source rows. That is what makes the same function serve
/// both the full-screen draw and the bounded-rect one: an image larger than
/// its box is clipped by LovyanGFX at no cost here, and the file still has to
/// be read sequentially either way.
bool drawRgb565FromSd(const String& path, int32_t boxX, int32_t boxY, int32_t boxW, int32_t boxH) {
  File file = SD.open(path.c_str());
  if (!file) {
    // sniffImageFormat() opened this a moment ago, so reaching here means the
    // card went away between the two - the same two-trips-to-the-filesystem
    // race drawImageFromSdInRect() documents for its Missing case.
    Log::printf("[display] %s vanished between identifying it and drawing it", path.c_str());
    return false;
  }

  uint8_t header[kRgb565HeaderBytes] = {0};
  if (static_cast<size_t>(file.read(header, sizeof(header))) != sizeof(header)) {
    Log::printf("[display] %s is too short to hold an rgb565 header (%u bytes)", path.c_str(),
                static_cast<unsigned>(file.size()));
    file.close();
    return false;
  }

  if (header[4] != kRgb565Version) {
    // Named as a version problem rather than a corruption problem, because the
    // two send a reader to completely different places. A future server
    // writing version 2 is this build being too old, and the honest remedy is
    // a firmware update, not an SD reformat.
    Log::printf("[display] %s is rgb565 version %u; this build reads version %u only", path.c_str(),
                static_cast<unsigned>(header[4]), static_cast<unsigned>(kRgb565Version));
    file.close();
    return false;
  }

  const bool littleEndianPixels = (header[5] & kRgb565FlagLittleEndianPixels) != 0;
  const int32_t width = (static_cast<int32_t>(header[6]) << 8) | header[7];
  const int32_t height = (static_cast<int32_t>(header[8]) << 8) | header[9];
  // Bytes 10..15 deliberately unread. Reserved means a reader ignores them; a
  // build that required them to be zero would refuse the first file written
  // after somebody uses one.

  const size_t expectedBytes =
      kRgb565HeaderBytes + (static_cast<size_t>(width) * height * kRgb565BytesPerPixel);
  if (width <= 0 || height <= 0 || file.size() != expectedBytes) {
    // The whole integrity story for a format with no checksum of its own. A
    // truncated write - an interrupted download that somehow survived
    // Assets.cpp's temp-file-then-rename, or a failing card - is a header
    // promising more pixels than are present, and drawing what arrived would
    // put a correct picture at the top of the panel and garbage below it,
    // which looks like a display fault rather than a storage one. Exact
    // rather than "at least", because trailing bytes mean the writer and this
    // reader disagree about something.
    Log::printf("[display] %s claims %dx%d (%u bytes) but holds %u - refusing to draw a partial "
                "picture",
                path.c_str(), (int)width, (int)height, static_cast<unsigned>(expectedBytes),
                static_cast<unsigned>(file.size()));
    file.close();
    return false;
  }

  // The band height, derived from the width rather than taken from
  // kRgb565BandLines outright - see kRgb565MaxBandBytes. At the panel's own
  // 320px this is the full 8 lines and 5,120 bytes; a wider image (which the
  // server does not currently produce) gets fewer lines rather than a malloc
  // this heap cannot serve. Never zero: one line is the floor, and a single
  // 320px line is 640 bytes.
  const size_t rowBytes = static_cast<size_t>(width) * kRgb565BytesPerPixel;
  size_t bandLines = kRgb565BandLines;
  while (bandLines > 1 && bandLines * rowBytes > kRgb565MaxBandBytes) {
    --bandLines;
  }
  const size_t bandBytes = bandLines * rowBytes;

  uint8_t* band = static_cast<uint8_t*>(malloc(bandBytes));
  if (band == nullptr) {
    // Reported with the heap figures, like every other allocation failure in
    // this file, because the interesting number is the largest CONTIGUOUS
    // block and not the free total - a distinction this file has paid for
    // twice (see logHeapSnapshot()).
    Log::printf("[display] no %u contiguous bytes for an rgb565 band buffer (8BIT largest=%u "
                "free=%u)",
                static_cast<unsigned>(bandBytes),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
    file.close();
    return false;
  }

  // Centred in the box, exactly as LovyanGFX's own middle_center datum centres
  // a decoded image - but with NO scaling, which is the deliberate difference.
  // An RGB565 asset is already at the size the server was asked to produce, and
  // there is nothing to scale with: unlike a decoder, this path cannot resample
  // on the way to the panel, and pretending otherwise would mean writing a
  // resampler here to work around a size the server can simply get right.
  const int32_t originX = boxX + ((boxW - width) / 2);
  const int32_t originY = boxY + ((boxH - height) / 2);

  // The clip rect is what makes an over-sized image safe and what lets this one
  // function serve both the full-panel and the bounded-rect draws. Set only
  // after every early return above, so there is exactly one place that has to
  // put it back.
  lcd.setClipRect(boxX, boxY, boxW, boxH);

  // Deliberately NOT wrapped in lcd.startWrite()/endWrite(). Holding one SPI
  // transaction across the whole frame would save 30 transaction setups, and
  // it should be safe - this file has established that the panel is on
  // HSPI_HOST while SdStorage.cpp's SD.begin() takes Arduino's default SPI on
  // VSPI, so an SD read inside a display transaction is not bus contention
  // (see drawImageFromSd()'s remarks on the shared-bus premise that did not
  // survive checking the pin assignments). It is left out because the saving
  // is negligible against 30 SD reads, while the failure mode if that
  // conclusion is ever wrong on some board is visible corruption on a
  // household's wall - and this whole path is uncompiled and unrun on
  // hardware as it stands. Worth revisiting once it has drawn a real picture.
  bool ok = true;
  for (int32_t y = 0; y < height && ok; y += static_cast<int32_t>(bandLines)) {
    const int32_t linesThisBand =
        (y + static_cast<int32_t>(bandLines) <= height) ? static_cast<int32_t>(bandLines)
                                                        : (height - y);
    const size_t wanted = static_cast<size_t>(linesThisBand) * rowBytes;

    if (static_cast<size_t>(file.read(band, wanted)) != wanted) {
      // A short read mid-picture, on a file whose length already checked out.
      // That is a card going bad rather than a bad file, and it is worth
      // saying which band it happened in: a consistent line number across
      // retries points at one place on the card.
      Log::printf("[display] short read %u bytes into the rgb565 band at line %d of %s",
                  static_cast<unsigned>(wanted), (int)y, path.c_str());
      ok = false;
      break;
    }

    if (littleEndianPixels) {
      // The unexercised branch - see this function's remarks. rgb565_t is the
      // little-endian layout, and handing it over as that type makes LovyanGFX
      // convert per pixel rather than copy, which is slower and correct.
      lcd.pushImage(originX, originY + y, width, linesThisBand,
                    reinterpret_cast<const lgfx::rgb565_t*>(band));
    } else {
      // The ordinary path: swap565_t is a byte-for-byte match for what this
      // panel writes, so this is a straight copy.
      lcd.pushImage(originX, originY + y, width, linesThisBand,
                    reinterpret_cast<const lgfx::swap565_t*>(band));
    }
  }

  lcd.clearClipRect();
  free(band);
  file.close();

  if (ok) {
    // Logged at verbose rather than unconditionally: this is the path that is
    // supposed to be boring, and a line per draw on a card that appears every
    // few minutes would crowd out the failures worth reading. The band figure
    // is in it because it is the number somebody tuning kRgb565BandLines will
    // want confirmed against a real device.
    Log::verbose("[display] drew %dx%d rgb565 from %s in %d bands of %u bytes (no decoder)",
                 (int)width, (int)height, path.c_str(),
                 (int)((height + (int32_t)bandLines - 1) / (int32_t)bandLines),
                 static_cast<unsigned>(bandBytes));
  }

  return ok;
}

/// The RAM-buffer twin of drawRgb565FromSd(), for the direct-to-RAM fallback.
///
/// **This should never run, and supporting it is still right.** A device with
/// no SD card is exactly the device that cannot hold one of these files: the
/// fetch needs the whole thing in one contiguous heap block, and 153,600 bytes
/// contiguous does not exist on this hardware. The server refuses the
/// assignment for a device that has reported having no card, so the bytes
/// should never get here. But "should never" is doing a lot of work in that
/// sentence - a device whose storage the server has not been told about is
/// only warned, not refused - and if the bytes DO arrive, they are already in
/// RAM and pushing them costs nothing at all: no band buffer, no read, one
/// call. Leaving the case out would instead fail as "unrecognised format",
/// which is a misleading thing to log about a file this build understands
/// perfectly well.
bool drawRgb565FromBuffer(const uint8_t* data, size_t size, int32_t boxX, int32_t boxY,
                          int32_t boxW, int32_t boxH) {
  if (data == nullptr || size < kRgb565HeaderBytes || data[4] != kRgb565Version) {
    Log::printf("[display] a %u-byte RAM buffer is not a readable rgb565 frame",
                static_cast<unsigned>(size));
    return false;
  }

  const bool littleEndianPixels = (data[5] & kRgb565FlagLittleEndianPixels) != 0;
  const int32_t width = (static_cast<int32_t>(data[6]) << 8) | data[7];
  const int32_t height = (static_cast<int32_t>(data[8]) << 8) | data[9];
  const size_t expectedBytes =
      kRgb565HeaderBytes + (static_cast<size_t>(width) * height * kRgb565BytesPerPixel);

  if (width <= 0 || height <= 0 || size != expectedBytes) {
    Log::printf("[display] a RAM rgb565 frame claims %dx%d (%u bytes) but holds %u",
                (int)width, (int)height, static_cast<unsigned>(expectedBytes),
                static_cast<unsigned>(size));
    return false;
  }

  const uint8_t* pixels = data + kRgb565HeaderBytes;
  const int32_t originX = boxX + ((boxW - width) / 2);
  const int32_t originY = boxY + ((boxH - height) / 2);

  // One push for the whole frame rather than bands: the bytes are already
  // contiguous in RAM, so banding would buy nothing and cost a loop.
  lcd.setClipRect(boxX, boxY, boxW, boxH);
  if (littleEndianPixels) {
    lcd.pushImage(originX, originY, width, height, reinterpret_cast<const lgfx::rgb565_t*>(pixels));
  } else {
    lcd.pushImage(originX, originY, width, height,
                  reinterpret_cast<const lgfx::swap565_t*>(pixels));
  }
  lcd.clearClipRect();

  Log::verbose("[display] drew %dx%d rgb565 straight from a RAM buffer (no decoder, no band copy)",
               (int)width, (int)height);
  return true;
}

}  // namespace

uint32_t consecutiveDrawFailures() { return gConsecutiveDrawFailures; }

/// Declared ahead of its definition below purely so the in-rect draw, which
/// comes first in this file, can call it too - see its own remarks further
/// down for why both PNG blocks are now handed back after every draw.
void releaseDecodeMemory();

bool drawImageFromSdInRect(const String& path, int32_t x, int32_t y, int32_t w, int32_t h) {
  // No fillScreen() here, deliberately - see this function's own header
  // comment. Streamed straight from SD, bounded to (w, h) at (x, y) instead of
  // the whole panel; scaleX/scaleY left at 0 is what makes LovyanGFX auto-fit
  // the image within that box rather than drawing it at native size.
  //
  // See drawImageFromSd() below for why this no longer reads the file into RAM
  // first. The in-rect case (the aircraft card's airline logo) is the smaller
  // of the two but was subject to exactly the same allocation.
  const ImageFormat format = sniffImageFormat(path);

  if (format == ImageFormat::Missing) {
    // No file, so no decode, so nothing this device failed at. Nothing has been
    // drawn and - unlike the full-screen path - nothing has been cleared
    // either, so the card the caller composed underneath this logo is left
    // exactly as it was, which is the right outcome for a logo that simply is
    // not cached. See noteNoImageAvailable() for why this is neither counted
    // nor allowed to clear a run of real failures.
    //
    // Assets::drawCachedInRect() normally keeps this unreachable by checking
    // isCached() first, but that check and this open are two separate trips to
    // the filesystem: a card pulled between them, or an invalidate() on another
    // path, lands here. An aircraft card redraws its logo on every appearance,
    // so a device in that state would otherwise have produced a steady drip of
    // counted "failures" for a picture nothing was ever going to find.
    noteNoImageAvailable("no cached file to draw in-rect", path);
    releaseDecodeMemory();
    return false;
  }

  bool ok = false;
  if (format == ImageFormat::Jpeg) {
    ok = lcd.drawJpgFile(SD, path.c_str(), x, y, w, h, 0, 0, 0.0f, 0.0f, middle_center);
  } else if (format == ImageFormat::Png) {
    ok = lcd.drawPngFile(SD, path.c_str(), x, y, w, h, 0, 0, 0.0f, 0.0f, middle_center);
  } else if (format == ImageFormat::Rgb565) {
    // Handled here for completeness rather than because it is expected. The
    // server only generates an RGB565 rung for asset types stored as JPEG -
    // the opaque full-card photographs - and this in-rect draw exists for
    // airline logos, which are a PNG type precisely because a logo layered
    // over an already-composed card needs real transparency that RGB565 does
    // not have. So an operator cannot currently choose one for this path. If
    // that ever changes, drawing it centred and clipped is the right
    // behaviour, and the alternative - falling into the branch below - would
    // report a file this build reads perfectly well as unrecognised bytes and
    // count it against the watchdog.
    ok = drawRgb565FromSd(path, x, y, w, h);
  } else {
    // Nothing drawn, and nothing guessed. Handing unrecognised bytes to a
    // decoder chosen by coin-flip cannot succeed - neither decoder will find a
    // header it understands - and it costs the one useful thing in the log,
    // because the failure would then be reported against a decoder instead of
    // against the file. Unlike the full-screen path this one has not cleared
    // the panel, so drawing nothing leaves the card underneath exactly as the
    // caller composed it, which is the right outcome for a missing logo.
    //
    // Counted as a failure all the same, and this is the case where that is
    // still right: the file opened, the bytes are here, and they are not a
    // picture. A device whose cache has filled up with unreadable files is in
    // exactly the state the watchdog exists to end. sniffImageFormat() has
    // already logged the header bytes.
    //
    // The case that USED to arrive here too, and should never have been counted,
    // was a file that would not open at all - handled above as
    // ImageFormat::Missing before this branch is reached.
  }

  noteDrawOutcome(ok);
  releaseDecodeMemory();
  if (!ok) {
    Log::printf(
        "[display] failed to draw %s (%s) in %dx%d rect at (%d,%d) (8BIT largest=%u free=%u)",
        path.c_str(), formatName(format), (int)w, (int)h, (int)x, (int)y,
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
  }
  return ok;
}

/// Hands back both large blocks a PNG draw needs - LovyanGFX's ~44KB decode
/// scratch and gFileBuffer's ~52KB copy of the file - the moment the draw is
/// finished with them.
///
/// THIS REVERSES AN EARLIER DELIBERATE DECISION, and the reasoning it reverses
/// is worth keeping rather than deleting, because it was sound as far as it
/// went. That decision (ported away from CYD-Dickey, which releases the
/// decoder unconditionally on every draw) argued: a ~44KB malloc/free cycle
/// per draw is repeated large-block churn, which fragments an ESP32 heap over
/// a long uptime; holding the buffer instead makes it a one-time bounded cost.
/// It also noted CAL has no Bluetooth stack to feed, which was CYD-Dickey's
/// stated reason for releasing.
///
/// What that reasoning missed is what those two blocks are taken *from*.
/// Measured on hardware, on a device with a splash and four picture cards:
///
///   [http] checkin failed - freeHeap=47072 maxAlloc=32756
///   [http] checkin: DNS ok, api.discoveraroundme.com -> 32.192.231.29
///   [http] checkin: plain TCP to :443 SUCCEEDED - the failure is the TLS
///                   handshake itself
///
/// mbedTLS needs roughly 16KB in + 16KB out plus certificate-parsing
/// workspace to complete a handshake. Holding ~96KB of decode buffers for the
/// whole uptime left 47KB free and 32,756 contiguous - enough to open a
/// socket, not enough to finish a handshake. Every HTTPS request on that
/// device failed: check-in AND the firmware manifest alike, so it could not
/// even be updated out of the state remotely. A second device on the same
/// account, firmware and server - but with no SD card and no picture cards,
/// so none of these buffers - checked in perfectly throughout.
///
/// So the trade is not "churn vs. no churn", it is "churn vs. no network".
/// Fragmentation over a long uptime is a real risk; a device that cannot
/// complete a TLS handshake at all is a present, total failure. Releasing
/// also means the buffers are only held during a draw, and check-ins happen
/// between draws, so the handshake sees an uncarved heap.
///
/// This is what CYD-Dickey has always done - the project whose graphics, as
/// its author points out, never had any of these problems.
void releaseDecodeMemory() {
  // Deliberately NOT releasePngMemory(). The two blocks look alike and are
  // not, and getting this split wrong breaks one subsystem or the other:
  //
  //   The decoder's ~44KB scratch is allocated ONCE, by the boot splash, at a
  //   point in setup() where maxAllocHeap is still 110,580 bytes - before
  //   WiFi and TLS carve the heap down. Because LovyanGFX keeps it unless
  //   asked not to, every later card draw REUSES it and never needs a large
  //   contiguous block again. Releasing it means the next draw must find 44KB
  //   in a post-WiFi heap whose largest hole is ~87KB minus whatever the file
  //   copy below just took - which is why releasing both made every graphic
  //   card fail even though check-ins started working:
  //
  //     [display] read ...4342155a.png into memory (54693 bytes)
  //     [display] failed to draw ... (freeHeap=46548 maxAllocHeap=32756)
  //
  //   That 32,756 is what was LEFT after the 54,693-byte file copy. The hole
  //   was big enough for one of them, never both.
  //
  //   gFileBuffer WAS the opposite case: a 31-54KB copy of the whole file,
  //   needed only for one drawPng() call. It is gone entirely - the draws
  //   stream from SD now, so there is no file copy to hold or release. See
  //   drawImageFromSd() for why the premise that required it (a shared SD and
  //   display SPI bus) did not survive checking the pin assignments.
  //
  // So what remains here is only the pngle scratch, and only the argument for
  // keeping it. There is no second block left to give back.
  //
  // Nothing was added here when the JPEG path arrived, and that is the point of
  // JPEG rather than an omission. draw_jpg() takes its whole 3,900-byte
  // workspace with malloc() and frees it on every exit path of its own
  // (LGFXBase.cpp:3051 against :3063, :3079 and :3106), retaining nothing
  // between draws - so there is no releaseJpgMemory() in the library and no
  // decision for this function to make. The asymmetry is worth naming because
  // it is easy to read as an oversight: a JPEG draw leaves this function
  // nothing to do, while a PNG draw leaves it something it deliberately
  // declines to do.
}

/// Streams the image straight off SD instead of reading it into RAM first,
/// through whichever decoder its own first bytes call for.
///
/// **Why there are now two decoders here at all.** Streaming (below) took the
/// per-draw file copy to approximately zero, and that was still not enough,
/// because LovyanGFX's PNG decoder is itself the expensive party. Measured on
/// hardware after the streaming change: at draw time the largest contiguous
/// 8BIT block fell to 5,876 bytes, and a 10,568-byte allocation failed outright
/// against a 6,132-byte block. The number that settles it is what a single
/// small draw costs - one 5,686-byte draw took the largest block from 12,276
/// down to 5,876. mbedTLS wants roughly 16KB in plus 16KB out for a handshake,
/// so after one graphic draw the device could no longer check in *or* report
/// why it could not, while still drawing the clock locally: silence that
/// presents to a household as a freeze. Device 7 - no SD card, no picture
/// cards, so this path never runs - stayed stable throughout on the same
/// builds, which is what says the graphics path and not the network stack is
/// where the memory went.
///
/// LovyanGFX's JPEG decoder is the way out, and the arithmetic is not close.
/// Its entire workspace is one 3,900-byte malloc, freed on every exit path,
/// with a 512-byte stream buffer inside it and nothing retained afterwards
/// (LGFXBase.cpp's draw_jpg: the malloc at :3051, the free at :3063, :3079 and
/// :3106). 3,900 fits under even the worst figure observed here - the 5,876
/// low-water mark - with room left over, there is no retained scratch, and
/// there is no releaseJpgMemory() for a future change to forget. The PNG
/// decoder by contrast keeps its pngle deliberately (see releaseDecodeMemory()
/// above for why that retention is load-bearing rather than an oversight) and
/// wants far more to set it up.
///
/// So the settled architecture is: photos become JPEG, logos that genuinely
/// need transparency stay PNG, and PNG support does not go anywhere - it stops
/// being the default. Which of the two runs is decided per file by
/// sniffImageFormat(), never by a wire field; that function's own comment has
/// the reasoning, and it is the part of this change most worth reading before
/// changing anything here.
///
/// What follows is the streaming decision this function was written for, kept
/// because it is still why neither decoder gets a file copy:
///
/// **This reverts the whole-file-buffer design, and the reason it existed
/// turned out not to be true.** readFileToBuffer() was introduced because this
/// board's SD card was believed to share its SPI bus with the display: on that
/// premise, LovyanGFX's own drawPngFile() was unusable, since its streaming
/// decode interleaves an SD read with a panel write for the whole length of the
/// image and would land on a contended bus for the duration. Reading the file
/// first separated the two in time.
///
/// The premise does not survive checking the pin assignments. The panel is on
/// HSPI_HOST at SCLK 14 / MISO 12 / MOSI 13 - see LovyanGFX's
/// _detector_Sunton_2432S028_9341_t, which also passes pin_tfcard_cs = -1, the
/// library's own assertion that no card sits on the panel's host - while
/// SdStorage.cpp's SD.begin() is handed no SPIClass and so takes Arduino's
/// default SPI object on VSPI. The board documentation's "shares SPI pins"
/// means the general SPI expansion header, which is both the more literal
/// reading and the only one consistent with both subsystems having worked all
/// along. SdStorage.cpp logs the pins in force at every mount so this
/// conclusion is checkable on any device rather than taken on trust.
///
/// What that premise cost: the buffer it justified is a 10-24KB CONTIGUOUS
/// allocation on every single draw, and that is the allocation that was
/// failing in the field. Measured at one such failure, the largest free 8BIT
/// block was 6,132 bytes against a 10,568-byte request, with 11,340 free in
/// total - so even a perfectly compacted heap would barely have served it.
/// Streaming takes peak NEW heap per draw to approximately zero: the decoder
/// works from the file a chunk at a time and needs no copy of it.
///
/// This is also what CYD-Dickey has always done, which is why its graphics
/// never had this problem - a fact that sat in this codebase's own comments as
/// an unexplained curiosity for some time.
bool drawImageFromSd(const String& path) {
  // Sniffed before the panel is cleared, so the log reads in the order things
  // happened: what the file is, then what the heap looked like going in.
  const ImageFormat format = sniffImageFormat(path);

  lcd.fillScreen(bg());

  if (format == ImageFormat::Missing) {
    // No file, so no decode. The panel has still been cleared, because that is
    // this function's standing contract on every false return (see the
    // declaration in Display.h) and the caller puts its own content back over a
    // clean panel either way - but the counter is left alone in both
    // directions. See noteNoImageAvailable() for why.
    noteNoImageAvailable("no cached file to draw", path);
    releaseDecodeMemory();
    restoreDefaultFont();
    return false;
  }

  // Kept across the streamed decode for now, not because a shared bus is
  // suspected any more but because these are the figures that will show
  // whether streaming actually removed the pressure - and now, separately,
  // whether JPEG's 3,900 bytes behave the way the source says they should on
  // a real heap. Worth deleting once a few devices have run clean.
  //
  // The decoder's name goes in the label rather than a generic "decode",
  // because which decoder ran is the first thing anyone reading a draw failure
  // needs to know and the last thing they can infer from a heap figure. The
  // PNG strings are unchanged from before this file learned about JPEG at all,
  // so a grep over older captured logs still lines up.
  const char* const beforeLabel = (format == ImageFormat::Jpeg)     ? "before drawJpgFile"
                                  : (format == ImageFormat::Rgb565) ? "before rgb565 bands"
                                                                    : "before drawPngFile";
  const char* const afterLabel = (format == ImageFormat::Jpeg)     ? "after drawJpgFile"
                                 : (format == ImageFormat::Rgb565) ? "after rgb565 bands"
                                                                   : "after drawPngFile";

  bool ok = false;
  if (format == ImageFormat::Unknown) {
    // Deliberately no snapshot pair around a decode that is not going to
    // happen: two identical heap lines with nothing between them would read
    // like a decode that consumed nothing, which is a different and much more
    // interesting finding than "the file was not an image". The screen has
    // already been cleared, which is this function's standing contract on
    // failure (see the declaration in Display.h): the caller gets false and
    // puts its own content back over a clean panel rather than a half-painted
    // one. sniffImageFormat() has already logged the offending bytes.
    //
    // Counted as a failure below, like any other draw where a real image was in
    // front of this device and no picture came out of it - see
    // drawImageFromSdInRect() for why that is the right reading of the counter
    // rather than an over-eager one. Reaching this branch now means the file
    // opened and its bytes are not an image; a file that would not open at all
    // returned above without touching the counter.
  } else if (format == ImageFormat::Rgb565) {
    // Snapshotted like the two decoders, and this is the pair most worth
    // capturing: the whole claim of this format is that a draw costs one
    // fixed 5,120-byte band buffer and gives it straight back, where the JPEG
    // path's 3,900 varies with nothing and the PNG path's ~45,056 is retained
    // for the process lifetime. The before/after figures either bear that out
    // on a real heap or they do not, and a claim in a comment is not a
    // measurement.
    //
    // The whole panel is the box here, so an image at the target size lands
    // centred exactly as a decoded one does - kScreenW/kScreenH rather than
    // LovyanGFX's own 0-means-the-whole-panel convention, because this path
    // does its own centring arithmetic and needs real numbers.
    //
    // A false return here IS counted against consecutiveDrawFailures() below,
    // and that is the right reading even for the cases that are not this
    // device's fault - a version this build cannot read, or a length that does
    // not match the header. The counter's narrow meaning is "an image was in
    // front of this device and no picture came out of it", which all of those
    // are; the case deliberately excluded is a file that was never there at
    // all, and that is handled as ImageFormat::Missing above before this branch
    // is reachable. A device whose cache has filled with files it cannot draw
    // is in exactly the state the watchdog exists to end, whichever end of the
    // rollout produced them.
    logHeapSnapshot(beforeLabel);
    ok = drawRgb565FromSd(path, 0, 0, kScreenW, kScreenH);
    logHeapSnapshot(afterLabel);
  } else {
    logHeapSnapshot(beforeLabel);
    ok = (format == ImageFormat::Jpeg)
             ? lcd.drawJpgFile(SD, path.c_str(), 0, 0, 0, 0, 0, 0, 0.0f, 0.0f, middle_center)
             : lcd.drawPngFile(SD, path.c_str(), 0, 0, 0, 0, 0, 0, 0.0f, 0.0f, middle_center);
    logHeapSnapshot(afterLabel);
  }

  noteDrawOutcome(ok);

  if (!ok) {
    Log::printf("[display] failed to draw %s (%s) (8BIT largest=%u free=%u)", path.c_str(),
                formatName(format),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
  }

  // Unconditional, including after a failed decode - a decode that ran out of
  // memory still leaves LovyanGFX's partial allocation behind, and that is
  // exactly the case where the memory is most needed back.
  releaseDecodeMemory();
  restoreDefaultFont();
  return ok;
}

bool drawImageFromBuffer(const uint8_t* data, size_t size) {
  lcd.fillScreen(bg());
  bool ok = false;
  ImageFormat format = ImageFormat::Unknown;
  if (data == nullptr || size == 0) {
    // No bytes, so no decode, so nothing this device failed at - the RAM-path
    // twin of the ImageFormat::Missing returns in the two SD draws above, and
    // not counted for the same reason. Assets::drawRam() keeps this unreachable
    // by checking the same two fields before its retry loop, but that guard
    // lives in another file and this function's own contract should not depend
    // on it: the counter is only meaningful if this file decides what counts,
    // and "nobody gave me a picture" is not a card this device failed to draw.
    noteNoImageAvailable("drawImageFromBuffer called with no data", String("RAM buffer"));
    restoreDefaultFont();
    return false;
  }

  // Sniffed exactly like the SD paths, and for a reason that is easy to miss:
  // a device with no usable card never writes to /assets at all, it fetches
  // straight to RAM (Assets::fetchToRam()). If this path stayed PNG-only,
  // adopting JPEG would silently blank the picture cards on precisely the
  // devices that have no cache to fall back on. classifyHeader() is shared
  // with sniffImageFormat() so the two paths cannot disagree about what a
  // JPEG looks like; there is no file to open here, the bytes are already in
  // front of us.
  //
  // classifyHeader() never returns ImageFormat::Missing and could not: that
  // enumerator means "no file would open", and this path has no file. The
  // no-image case here is the empty-buffer return above instead.
  format = classifyHeader(data, size);
  if (format == ImageFormat::Jpeg) {
    ok = lcd.drawJpg(data, size, 0, 0, 0, 0, 0, 0, 0.0f, 0.0f, middle_center);
  } else if (format == ImageFormat::Png) {
    ok = lcd.drawPng(data, size, 0, 0, 0, 0, 0, 0, 0.0f, 0.0f, middle_center);
  } else if (format == ImageFormat::Rgb565) {
    // Should be unreachable and handled anyway - see drawRgb565FromBuffer()
    // for the argument. Short version: a device taking this path is a device
    // with no SD card, which is exactly the device that cannot fetch one of
    // these files in the first place (the fetch wants all 153,600 bytes in one
    // contiguous heap block). The server refuses the assignment for a device
    // that has REPORTED having no card, but a device it has not heard from is
    // only warned - so if these bytes do arrive, drawing them costs nothing at
    // all, and the alternative is logging a file this build understands as
    // unrecognised.
    ok = drawRgb565FromBuffer(data, size, 0, 0, kScreenW, kScreenH);
  }
  // Nothing to release here: this path's caller owns the image bytes (a
  // RamAssetBuffer, see Assets.h) and it never fills gFileBuffer, while the
  // decoder's own scratch is deliberately kept for reuse - see
  // releaseDecodeMemory() for why that split is the load-bearing part.
  if (!ok) {
    Log::printf(
        "[display] failed to draw a %u-byte RAM buffer (%s) (freeHeap=%u maxAllocHeap=%u)",
        static_cast<unsigned>(size), formatName(format),
        static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  }

  // This path never fed the counter at all until recently. That was a gap, not
  // a decision - a no-SD device whose RAM draws all fail is as unable to show a
  // card as one whose SD draws all fail, and it was the only one of the three
  // draw entry points invisible to App.ino's watchdog. It cannot make the
  // watchdog trigger on a network problem: Assets.cpp does not reach this
  // function at all unless the fetch produced bytes, so getting here and
  // failing really is a decode failure and nothing else.
  //
  // **All of which is true, and all of which added up to a device rebooting
  // itself every eleven minutes.** Worth writing down in full, because the
  // conclusion is not the one the log line above suggests and two people have
  // now reached the wrong one from it.
  //
  // A device with no SD card takes this path for every graphic card it has -
  // Graphic.cpp's fetch() finds ensureCached() unavailable and falls back to
  // Assets::fetchToRam(), which works fine. What does not work is the decode
  // that follows, and the reason has nothing to do with the picture. LovyanGFX
  // keeps its ~44KB pngle scratch for the process lifetime once something
  // allocates it, and on a device with a card the thing that allocates it is
  // the boot splash, at a point in setup() where the largest block is still
  // 110,580 bytes. A card-less device never draws a splash at all -
  // Assets::showBootSplash() returns at its Sd::isReady() gate - so that
  // allocation never happens in the one roomy moment this firmware has, and
  // every later PNG decode has to find 44KB in a heap whose largest block is
  // the measured 21,000-26,000 of a device holding two RAM asset buffers. It
  // never can. The failure is permanent, identical on every retry, and
  // identical again on the next boot.
  //
  // What that looked like in the field, on device 7 (sdTotalBytes=0,
  // assetCount=0): Assets::drawRam() retries three times, so each graphic card
  // contributed 3 to this counter; two cards took it to exactly 6, where it sat
  // logging "6/6" once a minute without acting, because the limit is <=. Then
  // Graphic::draw() cleared each instance's gReady ("dropping this card for
  // now") and the cards left the rotation, so the counter stopped there. Ten
  // minutes later Config::kContentRefreshIntervalMs came due, fetch() re-fetched
  // both into RAM, both cards re-entered the rotation, and the next failed draw
  // was the 7th - restart, at 620-660s uptime, every single cycle. 97 boots to
  // 114 across one evening. The 10-minute refresh interval is what set that
  // period; the health check's own 60s cadence never had anything to do with it.
  //
  // The counter is still right to count this, and that is the point being
  // recorded rather than argued away: an image really was in front of the
  // decoder and no picture came out. What is wrong is upstream of this file -
  // a restart cannot reclaim memory for an allocation whose only affordable
  // moment is a splash the device is structurally unable to draw. The fixes
  // live in files this change does not own: give the card-less boot path
  // something that warms the pngle scratch early, or let the RAM path release
  // its own buffers and retry, or lean on JPEG (3,900 bytes of workspace, which
  // fits the 21,000-26,000 window with room to spare) for assets destined for
  // devices with no card. Until one of those lands, a card-less device with PNG
  // assets will keep earning this restart honestly.
  noteDrawOutcome(ok);
  restoreDefaultFont();
  return ok;
}

}  // namespace Display
