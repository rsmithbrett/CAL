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

// A score column holds two digits. The status column holds FINAL or a period on
// a row that has scores, and a start time on the one row that does not, and
// those are different widths: "10:00 PM" measures about 72px of
// FreeSansBold9pt7b against the 56 that fits FINAL, so the scoreless state
// takes its own figure with 8px of slack.
constexpr int kBoardScoreWidth = 26;
constexpr int kBoardStatusWidth = 56;
constexpr int kBoardColumnGap = 6;
constexpr int kBoardMinNameWidth = 24;

/// The measured width of the widest start time, and the slack the column adds to
/// it. "about 72px of FreeSansBold9pt7b" is this repository's own written figure
/// rather than anything a compiler can call textWidth() for, so the eight pixels
/// are slack on an approximate measurement and not room for a longer string.
///
/// Written as the sum rather than as 80, so a column trimmed towards the
/// measurement has to say which of the two it is taking the pixels from. The
/// host test reads both names.
constexpr int kBoardStartTimeTextWidth = 72;
constexpr int kBoardStartTimeSlack = 8;
constexpr int kBoardStartTimeWidth = kBoardStartTimeTextWidth + kBoardStartTimeSlack;

/// How far in from each edge of the panel the first and last column edge sit, on
/// top of kCardMargin.
constexpr int kBoardRowInset = 4;

/// Three blocks, all from one figure: whatever the row has left after the score
/// and status columns splits evenly between the two names. Every edge is derived
/// from the panel width, so a 240px panel lays out the same way a 320px one does
/// instead of inheriting literals that held at one width.
///
/// The state a row is in reaches the layout as `scoreless` and nothing else. A
/// game that has not started draws no score, so both score columns stand empty
/// in exactly the state whose status holds a start time, and the width they are
/// not using is what that column needs.
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

// The two panel widths on the fleet, checked by every build on both boards. The
// first pair is the width a start time was drawing in; TEXT_LAYOUT_DESIGN.md
// section 15 has the figures and TEST_PLAN.md section 18 the mutations they were
// proven against. They sit beside the arithmetic rather than in the host test,
// because an arduino-cli build of either board runs them and needs no host
// compiler to do it.
static_assert(boardColumns(320, true).statusWidth >= 72,
              "a scoreless row's status holds a start time, and '10:00 PM' is 72px");
static_assert(boardColumns(240, true).statusWidth >= 72,
              "a scoreless row's status holds a start time, and '10:00 PM' is 72px");
static_assert(boardColumns(320, false).nameWidth >= 83,
              "a row with scores keeps the 83px name column of v2026.10.31.0002");
static_assert(boardColumns(240, false).nameWidth >= 43,
              "a row with scores keeps the 43px name column of v2026.10.31.0002");

// The separation holds only while the kBoardMinNameWidth floor is not engaged:
// the floor clamps the name width and nothing clamps the edges derived from it,
// so below 174px scoreless and 202px with scores the home block crosses the
// status column. Both are under every panel the fleet has, and these two pairs
// are what says so.
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
