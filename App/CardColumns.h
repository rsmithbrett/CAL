#pragma once

/// The content margin every card measures from, and the scoreboard row's column
/// arithmetic.
///
/// Here rather than in Display.cpp because nothing below names a graphics
/// library, a font, a panel driver or an Arduino type. That is what lets a host
/// compiler build it. Display.cpp's own driver block includes LovyanGFX and a
/// board pin map, so the file holding these figures could only be compiled on
/// the toolchain for one of the two boards, and the only check the arithmetic
/// had was the static_asserts at the bottom of this file running inside an
/// arduino-cli build.
///
/// A static_assert reads a column's width at the two widths written into it. It
/// can't walk a range of panel widths to find where the kBoardMinNameWidth floor
/// engages, and it can't check that the figure this repository has measured for a
/// string fits the column drawn for that string. tests/card-columns-test.cpp does
/// both, and ci/test-card-columns.sh runs it. TEXT_LAYOUT_DESIGN.md section 15
/// has the derivation, TEST_PLAN.md section 18 the mutations.
namespace CardColumns {

constexpr int kCardMargin = 10;

/// The column edges of one scoreboard row. A name column takes its x as a left
/// edge and a score or status column takes its x as a right edge, which is what
/// Align does with it.
struct BoardColumns {
  int awayNameLeft;
  int awayScoreRight;
  int homeNameLeft;
  int homeScoreRight;
  int statusRight;
  int nameWidth;
  int scoreWidth;
  int statusWidth;
};

// A score column holds two digits. The status column holds a state word or a
// period on a row that has scores, and a start time on a row that has none, and
// those are not the same width.
constexpr int kBoardScoreWidth = 26;
constexpr int kBoardStatusWidth = 56;
constexpr int kBoardColumnGap = 6;
constexpr int kBoardMinNameWidth = 24;

/// The width the status column is sized against on a row with no scores, and
/// the slack it carries on top.
///
/// 78px is the widest string formatTimeOfDay() can hand it: "%d:%02d %s" with a
/// two-digit hour, and AM rather than PM, because A advances 13 and P advances
/// 12. Measured off the vendored FreeSansBold9pt7b glyph table by the method in
/// TEXT_LAYOUT_DESIGN.md section 9b, not estimated.
///
/// Written as the sum rather than as 80, so a column trimmed towards the
/// measurement has to say which of the two it is taking the pixels from. The
/// host test reads both names.
constexpr int kBoardStartTimeTextWidth = 78;
constexpr int kBoardStatusSlack = 2;
constexpr int kBoardStartTimeSlack = kBoardStatusSlack;
constexpr int kBoardStartTimeWidth = kBoardStartTimeTextWidth + kBoardStartTimeSlack;

/// What the 56px column on a row with scores is sized against. "FINAL" measures
/// 55px on the same table.
///
/// It is the floor the measured width in boardStatusWidthFor() below starts
/// from, and not a figure a period fits: the period is whatever progress text
/// the provider sent, and "6TH INN" is 74px, "4TH QTR" 79 and "HALFTIME" 94.
/// TEXT_LAYOUT_DESIGN.md section 15g has the figures.
constexpr int kBoardStateWordWidth = 55;

/// Two periods off the same glyph table, for the properties below to read.
///
/// "6TH INN" at 74px is the example TEXT_LAYOUT_DESIGN.md and Display.h both
/// use for what the status field holds. "HALFTIME" at 94px is the widest period
/// that can reach the panel at all: Sports::kMaxPeriodLength is 8 and
/// CheckIn.cpp's copyBoundedMarked() cuts what the provider sent down to it, so
/// "2ND PERIOD" arrives as "2ND P..." at 66px rather than as the 112px section
/// 15g measures.
constexpr int kBoardPeriodTextWidth = 74;
constexpr int kBoardWidestPeriodTextWidth = 94;

/// The narrowest a name column gets once the status column is taking its width
/// from the string it holds.
///
/// A choice about which column pays rather than a measurement, and the only
/// figure here that is one. It sits 7px clear of kBoardMinNameWidth, where the
/// clamp below engages and the separation identity stops holding, and it is the
/// largest floor that still leaves the 240px panel room to hand a start time
/// the 80px a start time measures. TEXT_LAYOUT_DESIGN.md section 15h has what
/// it costs each name column at each panel width.
constexpr int kBoardNameFloor = 31;

/// How far in from each edge of the panel the first and last column edge sit, on
/// top of kCardMargin.
constexpr int kBoardRowInset = 4;

/// Three blocks, all from one figure: whatever the row has left after the score
/// and status columns splits evenly between the two names. Every edge is derived
/// from the panel width, so a 240px panel lays out the same way a 320px one does
/// instead of inheriting literals that held at one width.
///
/// The row's state reaches the layout as `scoreless` and nothing else, and
/// `scoreless` means both score strings are empty. That is where the pixels
/// come from: two empty score columns release what the status column spends.
///
/// It is not the same thing as "the status holds a start time", and the two were
/// written here as though they were. A scheduled game is scoreless and does
/// hold a start time. A postponed game is also scoreless and holds "PPD" at
/// 37px, so it takes the 80px column and both names pay for it; a game whose
/// state the server did not send holds "" and takes the column for nothing. The
/// other direction costs more: Sports.cpp writes each side's score
/// independently, so a scheduled game with one score reported is not scoreless,
/// and its start time is handed the 56px column it does not fit.
/// TEXT_LAYOUT_DESIGN.md section 15f says what keying on the status instead
/// would take.
///
/// The two score columns a row gets, which is where the status column's pixels
/// come from.
constexpr int boardScoreWidth(bool scoreless) { return scoreless ? 0 : kBoardScoreWidth; }

/// The pixels between the row's first column edge and its last.
constexpr int boardRowSpan(int screenW) {
  return (screenW - kCardMargin - kBoardRowInset) - (kCardMargin + kBoardRowInset);
}

/// The status width a row's state asks for before anything measures its string.
/// What the two-argument boardColumns() below uses, and what the assertions at
/// the bottom of this file read.
constexpr int boardDefaultStatusWidth(bool scoreless) {
  return scoreless ? kBoardStartTimeWidth : kBoardStatusWidth;
}

/// The widest status column a row can be given: whatever leaves both name
/// columns at or above kBoardNameFloor, and never under the kBoardStatusWidth
/// the fleet already draws.
///
/// Nothing else bounds it. A ceiling sized against a string would be a guess at
/// the provider's vocabulary, and the widest eight characters the glyph table
/// holds is 144px, so the names are what says no.
constexpr int boardMaxStatusWidth(int screenW, bool scoreless) {
  const int fromNames = boardRowSpan(screenW) - 2 * boardScoreWidth(scoreless) -
                        3 * kBoardColumnGap - 2 * kBoardNameFloor;
  return fromNames > kBoardStatusWidth ? fromNames : kBoardStatusWidth;
}

/// The status column for a status that measures `measuredTextWidth` pixels in
/// the face it draws in.
///
/// An empty status asks for nothing, because nothing is drawn there. A status
/// that fits the 56px column keeps the 56px column, so a state word costs the
/// names no pixels. Anything wider asks for its own width plus the slack, up to
/// boardMaxStatusWidth().
///
/// This is what reaches both doors in TEXT_LAYOUT_DESIGN.md sections 15f and
/// 15g: a start time on a row that reported one score, and a live game's period
/// on a row that reported both. Section 15h has the before-and-after widths and
/// what the name columns pay for them.
constexpr int boardStatusWidthFor(int screenW, bool scoreless, int measuredTextWidth) {
  if (measuredTextWidth <= 0) { return 0; }
  if (measuredTextWidth <= kBoardStatusWidth) { return kBoardStatusWidth; }
  const int want = measuredTextWidth;
  const int ceiling = boardMaxStatusWidth(screenW, scoreless);
  return want < ceiling ? want : ceiling;
}

/// constexpr so the properties below are checked by every build on both panels.
constexpr BoardColumns boardColumns(int screenW, bool scoreless, int statusWidth) {
  const int rowLeft = kCardMargin + kBoardRowInset;
  const int rowRight = screenW - kCardMargin - kBoardRowInset;

  const int scoreWidth = boardScoreWidth(scoreless);

  // Everything that is not a name. Three column gaps for the two that sit
  // between the three blocks: the third is what boardSeparation() below reads
  // back out as the air between the home score and the status.
  //
  // The status width is charged here as well as drawn. Charging a fixed figure
  // and drawing a wider one lets a scoreless row's names and its status column
  // both spend the pixels the score columns released.
  const int fixedWidth = 2 * scoreWidth + statusWidth + 3 * kBoardColumnGap;
  const int shared = (rowRight - rowLeft - fixedWidth) / 2;
  const int nameWidth = shared > kBoardMinNameWidth ? shared : kBoardMinNameWidth;

  const int awayNameLeft = rowLeft;
  const int awayScoreRight = awayNameLeft + nameWidth + scoreWidth;
  const int homeNameLeft = awayScoreRight + kBoardColumnGap;
  const int homeScoreRight = homeNameLeft + nameWidth + scoreWidth;

  return BoardColumns{awayNameLeft, awayScoreRight, homeNameLeft, homeScoreRight,
                      rowRight,     nameWidth,      scoreWidth,   statusWidth};
}

/// The row with the status width its state asks for. The draw path measures the
/// string instead and calls the three-argument form above.
constexpr BoardColumns boardColumns(int screenW, bool scoreless) {
  return boardColumns(screenW, scoreless, boardDefaultStatusWidth(scoreless));
}

constexpr int boardStatusLeft(int screenW, bool scoreless, int statusWidth) {
  const BoardColumns columns = boardColumns(screenW, scoreless, statusWidth);
  return columns.statusRight - columns.statusWidth;
}

constexpr int boardStatusLeft(int screenW, bool scoreless) {
  return boardStatusLeft(screenW, scoreless, boardDefaultStatusWidth(scoreless));
}

/// What stands between the home block's right edge and the status column's
/// left edge. Substituting `shared` back gives 2 * kBoardColumnGap for any
/// status width, score width and panel width, plus the one pixel the even split
/// cannot divide when the row's leftover is odd, which is why the two columns
/// cannot be made to touch by changing one of those figures.
constexpr int boardSeparation(int screenW, bool scoreless, int statusWidth) {
  return boardStatusLeft(screenW, scoreless, statusWidth) -
         boardColumns(screenW, scoreless, statusWidth).homeScoreRight;
}

constexpr int boardSeparation(int screenW, bool scoreless) {
  return boardSeparation(screenW, scoreless, boardDefaultStatusWidth(scoreless));
}

/// The pixels the even split could not divide between the two names. They land
/// in the separation above, and whose parity they carry is the row's leftover,
/// not the panel width's: every status width in this file was even until one
/// was measured off a string. TEXT_LAYOUT_DESIGN.md section 15c.
constexpr int boardLeftoverPixel(int screenW, bool scoreless, int statusWidth) {
  return (boardRowSpan(screenW) -
          (2 * boardScoreWidth(scoreless) + statusWidth + 3 * kBoardColumnGap)) &
         1;
}

// The two panel widths on the fleet, checked by every build on both boards.
// They read the literals 320 and 240, so they speak for the two shipped panels
// and for no other width; the sweep in tests/card-columns-test.cpp is what
// covers a width nobody has built. TEXT_LAYOUT_DESIGN.md section 15 has the
// figures and TEST_PLAN.md section 18 the mutations they were proven against.
// They sit beside the arithmetic rather than in the host test, because an
// arduino-cli build of either board runs them and needs no host compiler.
static_assert(boardColumns(320, true).statusWidth >= kBoardStartTimeTextWidth,
              "a scoreless row's status holds a start time, and the widest is 78px");
static_assert(boardColumns(240, true).statusWidth >= kBoardStartTimeTextWidth,
              "a scoreless row's status holds a start time, and the widest is 78px");
static_assert(boardColumns(320, false).statusWidth >= kBoardStateWordWidth,
              "a row with scores draws a state word there, and FINAL is 55px");
static_assert(boardColumns(240, false).statusWidth >= kBoardStateWordWidth,
              "a row with scores draws a state word there, and FINAL is 55px");
static_assert(boardColumns(320, false).nameWidth >= 83,
              "a row with scores keeps the 83px name column of v2026.10.31.0002");
static_assert(boardColumns(240, false).nameWidth >= 43,
              "a row with scores keeps the 43px name column of v2026.10.31.0002");

// The separation holds only while the kBoardMinNameWidth floor is not engaged:
// the floor clamps the name width and nothing clamps the edges derived from it,
// so below 174px scoreless and 202px with scores the separation falls under
// 2 * kBoardColumnGap, and below 162 and 190 the home block and the status
// column overlap. All four are under every panel the fleet has, and these two
// pairs are what says so at the two it has.
static_assert(boardColumns(320, false).nameWidth > kBoardMinNameWidth, "320 clears the floor");
static_assert(boardColumns(240, false).nameWidth > kBoardMinNameWidth, "240 clears the floor");
static_assert(boardSeparation(320, true) >= 2 * kBoardColumnGap, "320 scoreless stays apart");
static_assert(boardSeparation(320, false) >= 2 * kBoardColumnGap, "320 scored stays apart");
static_assert(boardSeparation(240, true) >= 2 * kBoardColumnGap, "240 scoreless stays apart");
static_assert(boardSeparation(240, false) >= 2 * kBoardColumnGap, "240 scored stays apart");

// Right edge inside the content band at both widths, which is where the row of
// literals this replaced put 248 on a 240px panel.
static_assert(boardColumns(320, true).statusRight <= 320 - kCardMargin, "320 is on the glass");
static_assert(boardColumns(240, true).statusRight <= 240 - kCardMargin, "240 is on the glass");

// The measured column, as the two properties TEXT_LAYOUT_DESIGN.md sections 15g
// and 15f are each about. Both read boardStatusWidthFor() at a figure measured
// off the glyph table, which is the whole claim: the column holds the string it
// was given.
static_assert(boardStatusWidthFor(320, false, kBoardPeriodTextWidth) >= kBoardPeriodTextWidth,
              "a live row with scores draws a period, and 6TH INN is 74px");
static_assert(boardStatusWidthFor(240, false, kBoardPeriodTextWidth) >= kBoardPeriodTextWidth,
              "a live row with scores draws a period, and 6TH INN is 74px");
static_assert(boardStatusWidthFor(320, true, kBoardPeriodTextWidth) >= kBoardPeriodTextWidth,
              "a live row with no score sent draws a period too");
static_assert(boardStatusWidthFor(240, true, kBoardPeriodTextWidth) >= kBoardPeriodTextWidth,
              "a live row with no score sent draws a period too");
static_assert(boardStatusWidthFor(320, false, kBoardStartTimeTextWidth) >= kBoardStartTimeTextWidth,
              "a scheduled row that reported one score still holds a start time");
static_assert(boardStatusWidthFor(240, false, kBoardStartTimeTextWidth) >= kBoardStartTimeTextWidth,
              "a scheduled row that reported one score still holds a start time");

// And the other half of the trade: a state word that already fits leaves the
// name columns exactly where v2026.10.31.0002 put them.
static_assert(boardColumns(320, false, boardStatusWidthFor(320, false, kBoardStateWordWidth))
                      .nameWidth == 83,
              "FINAL costs the 320px panel's names no pixels");
static_assert(boardColumns(240, false, boardStatusWidthFor(240, false, kBoardStateWordWidth))
                      .nameWidth == 43,
              "FINAL costs the 240px panel's names no pixels");

// The ceiling never asks for a column narrower than the one the fleet draws,
// and at the ceiling both name columns are still on their floor rather than
// through it.
static_assert(boardMaxStatusWidth(320, false) >= kBoardStatusWidth, "320 keeps its 56");
static_assert(boardMaxStatusWidth(240, false) >= kBoardStatusWidth, "240 keeps its 56");
static_assert(boardColumns(320, false, boardMaxStatusWidth(320, false)).nameWidth >=
                  kBoardNameFloor,
              "320 with scores holds the name floor at its ceiling");
static_assert(boardColumns(240, false, boardMaxStatusWidth(240, false)).nameWidth >=
                  kBoardNameFloor,
              "240 with scores holds the name floor at its ceiling");
static_assert(boardColumns(320, true, boardMaxStatusWidth(320, true)).nameWidth >= kBoardNameFloor,
              "320 scoreless holds the name floor at its ceiling");
static_assert(boardColumns(240, true, boardMaxStatusWidth(240, true)).nameWidth >= kBoardNameFloor,
              "240 scoreless holds the name floor at its ceiling");

}  // namespace CardColumns
