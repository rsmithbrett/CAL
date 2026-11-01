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
constexpr int kBoardStartTimeSlack = 2;
constexpr int kBoardStartTimeWidth = kBoardStartTimeTextWidth + kBoardStartTimeSlack;

/// What the 56px column on a row with scores is sized against. "FINAL" measures
/// 55px on the same table.
///
/// It is not sized against a period. The period is whatever progress text the
/// provider sent, up to the 15 characters SportsBoardRow::kMaxStatus holds, and
/// "6TH INN" is 74px, "4TH QTR" 79 and "HALFTIME" 94. Those do not fit and are
/// ellipsized. TEXT_LAYOUT_DESIGN.md section 15g has the figures and what
/// changing it would cost.
constexpr int kBoardStateWordWidth = 55;

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
/// constexpr so the properties below are checked by every build on both panels.
constexpr BoardColumns boardColumns(int screenW, bool scoreless) {
  const int rowLeft = kCardMargin + kBoardRowInset;
  const int rowRight = screenW - kCardMargin - kBoardRowInset;

  const int scoreWidth = scoreless ? 0 : kBoardScoreWidth;
  const int statusWidth = scoreless ? kBoardStartTimeWidth : kBoardStatusWidth;

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

constexpr int boardStatusLeft(int screenW, bool scoreless) {
  const BoardColumns columns = boardColumns(screenW, scoreless);
  return columns.statusRight - columns.statusWidth;
}

/// What stands between the home block's right edge and the status column's
/// left edge. Substituting `shared` back gives 2 * kBoardColumnGap for any
/// status width, score width and panel width, which is why the two columns
/// cannot be made to touch by changing one of those figures.
constexpr int boardSeparation(int screenW, bool scoreless) {
  return boardStatusLeft(screenW, scoreless) - boardColumns(screenW, scoreless).homeScoreRight;
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

}  // namespace CardColumns
