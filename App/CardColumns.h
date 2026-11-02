#pragma once

#include <stdint.h>

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
///
/// The status column's width comes from the string it holds. Section 15h is the
/// shape and section 15g is what the fixed width cut. The board gives its
/// status column the width its widest string asks for and lays every row on
/// the one grid that leaves, which is section 15i. That grid is not always one
/// of the grids its rows would have asked for on their own, because the score state
/// is the board's question and not a row's.
///
/// Nothing here reads App/Display.cpp, so the arguments the draw path hands
/// these functions are checked by ci/check-board-columns-call.sh instead, a
/// text gate over showSportsBoard() and the fold that feeds it. TEST_PLAN.md
/// sections 18e and 18f have the single-token changes to those lines that each
/// drew a wrong board with nothing in this file firing.
namespace CardColumns {

constexpr int kCardMargin = 10;

/// What the board's scores need from the two score columns, which is the one
/// input the layout takes beyond the panel width and the status string.
///
/// Three cases rather than the `BoardScores scores` this replaced, because the
/// question is three-valued: a board where no row reported either side, one
/// whose widest score is two digits, and one carrying a three-digit score.
/// TEXT_LAYOUT_DESIGN.md section 15l is why the third exists, and what it costs.
///
/// An enum class rather than a second bool beside the first. Two bools admit a
/// fourth combination that means nothing - scores and three-digit at once -
/// and let a caller swap them, which is the hazard the remark beside
/// boardColumns() below already records about an int converting to bool without
/// a diagnostic. An enum class converts from nothing, so a forgotten or swapped
/// argument stops the compile rather than drawing a wrong board.
///
/// It is the BOARD's state and not a row's, for the reason
/// boardColumnsForBoard() gives: one game under way gives all four rows a score
/// column, and one score over 99 gives all four rows the wider one, so the
/// digits down the right of each block stay in one column.
enum class BoardScores : uint8_t { None, TwoDigit, ThreeDigit };

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

// A score column holds two digits of FreeSansBold12pt7b, which is 28 and not
// the 26 two of them average: kBoardWidestTwoDigitScoreWidth below is that
// measurement and the static_assert at the bottom of this file reads the two
// against each other. It was 26 until 2026-11-02, so eighteen of the ninety
// two-digit scores were a tier small on the panel - TEXT_LAYOUT_DESIGN.md
// section 15k.
//
// kBoardStatusWidth is the narrowest status column the row ever draws and the
// floor boardStatusWidthFor() starts from; what it holds is a state word, a
// period or a start time, and those are not the same width, so the width comes
// from the string rather than from here. kBoardMinNameWidth is the clamp inside
// boardColumns(), below which the edges derived from the name width stop
// holding - a different figure from kBoardNameFloor, which is what the status
// column's ceiling is budgeted against.
constexpr int kBoardScoreWidth = 28;
constexpr int kBoardStatusWidth = 56;
constexpr int kBoardColumnGap = 6;
constexpr int kBoardMinNameWidth = 24;

/// The width a start time measures, and the slack a measured column carries on
/// top of its string. kBoardStartTimeWidth is the default a row with no scores
/// gets before anything measures, and the column boardStatusWidthFor() returns
/// for a start time at either panel width.
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
/// use for what the status field holds.
///
/// "HALFTIME" at 94px is the widest period a provider plausibly sends that the
/// 8-character Sports::kMaxPeriodLength holds whole, and it is not the widest
/// eight characters: "OVERTIME" is 97 and the widest the glyph table can make is
/// 144, which is why boardMaxStatusWidth() below is derived from the names
/// rather than from a string. What the cap does reach is anything longer, which
/// CheckIn.cpp's copyBoundedMarked() cuts and marks, so "2ND PERIOD" arrives as
/// "2ND P..." at 66px rather than as the 112px section 15g measures.
constexpr int kBoardPeriodTextWidth = 74;
constexpr int kBoardWidestPeriodTextWidth = 94;

/// What a score column has to hold, off the vendored glyph tables by the method
/// TEXT_LAYOUT_DESIGN.md section 9b describes.
///
/// kBoardWidestTwoDigitScoreWidth is the widest two digits of
/// FreeSansBold12pt7b, which is "11" at 28. `1` is the only digit in that face
/// whose xAdvance is 14 rather than 13, so a two-digit score carrying one is 27
/// or 28 and the rest are 26: eighteen of the ninety two-digit scores are over
/// 26 and none is over 28. A single `1` is 14 and was never the problem.
///
/// kBoardWidestTwoDigitStackedScoreWidth is the same question at
/// FreeSansBold9pt7b, where every digit advances 10 and `1` is not special, so
/// any two of them are 20. Section 15k is what each of these two columns was
/// drawing before they were measured against.
///
/// Three digits are over both columns and over every tier the fitting ladder
/// has - "100" is 40 at Sans12 and 30 at Sans9 - and 15k records that as open
/// rather than closed.
constexpr int kBoardWidestTwoDigitScoreWidth = 28;
constexpr int kBoardWidestTwoDigitStackedScoreWidth = 20;

/// The column a three-digit score needs, and the one a four-digit score would.
///
/// kBoardThreeDigitScoreWidth is the widest three digits of FreeSansBold9pt7b,
/// which is 30 and is also the narrowest: every digit of that face advances 10,
/// so all nine hundred three-digit scores are exactly 30 and one column holds
/// the lot. At Sans12 they run 39 to 42, and a 42px column takes 14px off every
/// name on the board instead of 2 - TEXT_LAYOUT_DESIGN.md section 15l costs
/// that out and takes the face instead, so a board carrying a three-digit score
/// draws every score on it at Sans9.
///
/// kBoardWidestFourDigitScoreWidth is the same question one digit further out,
/// and it is here so the gap is a figure a run reads rather than a sentence: 40
/// is over the column, so a four-digit score draws an ellipsis the way a
/// three-digit one did before 15l. No sport this card carries sends one.
constexpr int kBoardThreeDigitScoreWidth = 30;
constexpr int kBoardWidestFourDigitScoreWidth = 40;

/// A single-digit-hour start time, off the same glyph table: "6:00 PM" and
/// "7:30 PM" are both 67px, against the 78 kBoardStartTimeTextWidth carries for
/// the widest one formatTimeOfDay() can make. It is the ordinary evening
/// fixture and the status the photographed board in TEXT_LAYOUT_DESIGN.md
/// section 15j was widest on.
constexpr int kBoardShortStartTimeTextWidth = 67;

/// The narrowest a name column gets once the status column is taking its width
/// from the string it holds.
///
/// A choice about which column pays rather than a measurement, and the only
/// figure here that is one. It sits 5px clear of kBoardMinNameWidth, where the
/// clamp below engages and the separation identity stops holding, and it is the
/// largest floor that still leaves the 240px panel room to hand a start time
/// the 80px a start time measures. TEXT_LAYOUT_DESIGN.md section 15h has what
/// it costs each name column at each panel width.
///
/// 29 rather than the 31 it was until 2026-11-02, which is the same derivation
/// re-run: the largest floor clearing a start time depends on the score column,
/// and the score column is 28 now. At 31 the 240px ceiling would be 76 and a
/// start time would be cut there again. Section 15k has the trade and the eight
/// ceilings it moves.
constexpr int kBoardNameFloor = 29;

/// How far in from each edge of the panel the first and last column edge sit, on
/// top of kCardMargin.
constexpr int kBoardRowInset = 4;

/// The two score columns a row gets, which is where the status column's pixels
/// come from.
///
/// BoardScores::None means every score string on the board is empty, which is
/// not the same thing as "the status holds a start time": Sports.cpp writes each
/// side independently, so a scheduled game with one score reported is not None.
/// That is why the status width is measured rather than taken from this.
/// TEXT_LAYOUT_DESIGN.md sections 15f and 15h.
///
/// The other two are what the widest score on the board needs: two digits of
/// Sans12 on a one-line row, or three digits of Sans9 on either shape, because
/// a three-digit board drops its score face. Section 15l is the trade.
constexpr int boardScoreWidth(BoardScores scores) {
  return scores == BoardScores::None
             ? 0
             : (scores == BoardScores::ThreeDigit ? kBoardThreeDigitScoreWidth
                                                  : kBoardScoreWidth);
}

/// The pixels between the row's first column edge and its last.
constexpr int boardRowSpan(int screenW) {
  return (screenW - kCardMargin - kBoardRowInset) - (kCardMargin + kBoardRowInset);
}

/// The status width a row's state asks for before anything measures its string.
/// What boardColumns() below uses, and what the fourteen assertions predating
/// section 15h read.
constexpr int boardDefaultStatusWidth(BoardScores scores) {
  return scores == BoardScores::None ? kBoardStartTimeWidth : kBoardStatusWidth;
}

/// The widest status column a row can be given: whatever leaves both name
/// columns at or above kBoardNameFloor, and never under the kBoardStatusWidth
/// the fleet already draws.
///
/// Nothing else bounds it. A ceiling sized against a string would be a guess at
/// the provider's vocabulary, and the widest eight characters the glyph table
/// holds is 144px, so the names are what says no.
constexpr int boardMaxStatusWidth(int screenW, BoardScores scores) {
  const int fromNames = boardRowSpan(screenW) - 2 * boardScoreWidth(scores) -
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
constexpr int boardStatusWidthFor(int screenW, BoardScores scores, int measuredTextWidth) {
  if (measuredTextWidth <= 0) { return 0; }
  if (measuredTextWidth <= kBoardStatusWidth) { return kBoardStatusWidth; }
  const int want = measuredTextWidth + kBoardStatusSlack;
  const int ceiling = boardMaxStatusWidth(screenW, scores);
  return want < ceiling ? want : ceiling;
}

/// Three blocks, all from one figure: whatever the row has left after the score
/// and status columns splits evenly between the two names. Every edge is derived
/// from the panel width, so a 240px panel lays out the same way a 320px one does
/// instead of inheriting literals that held at one width.
///
/// Two inputs beyond the panel width. `scores` picks the score columns, which
/// is where the pixels come from: two empty score columns release what the
/// status column spends. `statusWidth` is what the status column takes, and the
/// caller has to pass the same figure it draws into - boardStatusWidthFor()
/// above is what produces it from the string.
///
/// constexpr so the properties below are checked by every build on both panels.
constexpr BoardColumns boardColumnsFor(int screenW, BoardScores scores, int statusWidth) {
  const int rowLeft = kCardMargin + kBoardRowInset;
  const int rowRight = screenW - kCardMargin - kBoardRowInset;

  const int scoreWidth = boardScoreWidth(scores);

  // Everything that is not a name. Three column gaps for the two that sit
  // between the three blocks: the third is what boardSeparation() below reads
  // back out as the air between the home score and the status.
  //
  // The status width is charged here as well as drawn, which is why the caller
  // passes one figure for both. Charging a narrower one than it draws lets the
  // names and the status column both spend the same pixels.
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

/// The row with the status width its state asks for, which is what a row would
/// take if it were the only one on the card. Nothing on the draw path calls it:
/// showSportsBoard() measures the board's strings and calls
/// boardColumnsForBoard() below, and what reads this are the assertions at the
/// bottom of this file and the host test.
///
/// Its own name rather than a two-argument overload of that one. An int converts
/// to bool without a diagnostic, so `boardColumns(240, measuredWidth)` written
/// by somebody reaching for the measured form would compile, discard the
/// measurement and return the scores layout - and every assertion in this
/// file and every case in the host test would pass, because none of them reads
/// the draw path. Two arities under one name is the shape that allows that;
/// two names do not.
constexpr BoardColumns boardColumns(int screenW, BoardScores scores) {
  return boardColumnsFor(screenW, scores, boardDefaultStatusWidth(scores));
}

/// The one grid every row of a board draws into.
///
/// `scores` is the board's rather than a row's: None only when no row of it
/// reported either score, and ThreeDigit as soon as one row did reach three
/// digits. One game under way gives all four rows a score column, one score
/// over 99 gives all four rows the wider one, and a row with no score of its
/// own draws nothing into whichever it got.
///
/// `widestStatusTextWidth` is the widest any of the board's statuses measures,
/// not the widest column any of them asks for. boardStatusWidthFor() does not
/// fall as its measurement rises, so those two are the same column and the
/// caller can fold the measurements and convert once. The host sweep asserts
/// that rather than leaving it as the reason this remark gives.
///
/// TEXT_LAYOUT_DESIGN.md section 15i. 15h's table is what each row would have
/// asked for alone; the board picks the widest status of them, and then
/// re-derives the rest from its own `scores`, so the grid it lands on need
/// not be any row's.
constexpr BoardColumns boardColumnsForBoard(int screenW, BoardScores scores,
                                            int widestStatusTextWidth) {
  return boardColumnsFor(screenW, scores,
                         boardStatusWidthFor(screenW, scores, widestStatusTextWidth));
}

/// The brace that joins a two-line row's two lines, and the score column those
/// lines draw into.
///
/// kBoardBraceWidth is 8 at any height. The nose is the leftmost pixel, the
/// spine stands 4px right of it, and each tip runs 3px right of the spine;
/// Display.cpp's drawRowBrace() asserts those three add up to this. It is drawn
/// from line segments rather than set as a glyph, because the widest brace
/// either vendored face carries is 17px tall against a 44px block.
///
/// kBoardStackedScoreWidth is two digits of FreeSansBold9pt7b, measured off the
/// vendored glyph table by the method TEXT_LAYOUT_DESIGN.md section 9b
/// describes. kBoardScoreWidth's 28 is the same two digits at Sans12, which is
/// the face a one-line row draws them in and a two-line row has no height for:
/// two Sans12 slots are 58px against the 52px card the profile panel gives a
/// row. Section 15j is the trade.
///
/// 20 holds any two digits of that face and always did. Every digit of Sans9
/// advances 10, where Sans12 gives `1` an xAdvance of 14 against every other
/// digit's 13, so section 15k's score defect reached the one-line row and not
/// this one.
constexpr int kBoardBraceWidth = 8;
constexpr int kBoardStackedScoreWidth = 20;

/// The brace's left edge, which is the row's own left edge.
constexpr int boardStackedBraceLeft() { return kCardMargin + kBoardRowInset; }

/// The one score column a two-line row draws, which both of its lines share.
/// Released by the board's `scores` on the same rule boardScoreWidth() uses.
///
/// A three-digit board asks for the same 30 the one-line row asks for, because
/// both draw three digits of Sans9 there: the two-line row already drew its
/// scores in that face, so the three-digit state costs it the 10px between 20
/// and 30 rather than the 2px the one-line row pays. Section 15l has what that
/// is worth and which name it moves.
constexpr int boardStackedScoreWidth(BoardScores scores) {
  return scores == BoardScores::None
             ? 0
             : (scores == BoardScores::ThreeDigit ? kBoardThreeDigitScoreWidth
                                                  : kBoardStackedScoreWidth);
}

/// The widest status column a two-line row can be given.
///
/// boardMaxStatusWidth()'s shape with one kBoardNameFloor subtracted instead of
/// two, because a two-line row has one name column where a one-line row has
/// two. That single subtraction is what takes HALFTIME off the list of strings
/// the 240px panel cannot hold: 94px asks for 96, the one-line ceiling there is
/// 80, and this one is 137.
constexpr int boardStackedMaxStatusWidth(int screenW, BoardScores scores) {
  const int fromName = boardRowSpan(screenW) - kBoardBraceWidth -
                       boardStackedScoreWidth(scores) - 3 * kBoardColumnGap -
                       kBoardNameFloor;
  return fromName > kBoardStatusWidth ? fromName : kBoardStatusWidth;
}

/// The three rules boardStatusWidthFor() states, under the two-line ceiling.
/// Nothing is drawn for an empty status, a status that fits the 56px column
/// keeps it, and anything wider asks for its own width plus the slack.
constexpr int boardStackedStatusWidthFor(int screenW, BoardScores scores,
                                         int measuredTextWidth) {
  if (measuredTextWidth <= 0) { return 0; }
  if (measuredTextWidth <= kBoardStatusWidth) { return kBoardStatusWidth; }
  const int want = measuredTextWidth + kBoardStatusSlack;
  const int ceiling = boardStackedMaxStatusWidth(screenW, scores);
  return want < ceiling ? want : ceiling;
}

/// A two-line row's column edges: a brace, one name, one score and the status.
///
/// Returns the same BoardColumns a one-line row does, with the away and home
/// edges equal, because the two lines sit at one x and differ in y alone. That
/// is what lets showSportsBoard() keep one set of layoutLine() calls for both
/// orientations instead of forking the draw path.
///
/// The name takes the whole remainder rather than half of one, so there is no
/// pixel a split cannot divide: boardStackedSeparationFor() below is exactly
/// 2 * kBoardColumnGap at every panel width, where the one-line form carries
/// the parity term boardLeftoverPixel() reads.
constexpr BoardColumns boardStackedColumnsFor(int screenW, BoardScores scores,
                                              int statusWidth) {
  const int rowLeft = boardStackedBraceLeft();
  const int rowRight = screenW - kCardMargin - kBoardRowInset;
  const int scoreWidth = boardStackedScoreWidth(scores);
  const int nameLeft = rowLeft + kBoardBraceWidth + kBoardColumnGap;

  // Three column gaps for the three that sit between the four blocks, one of
  // which is the air boardStackedSeparationFor() reads back out.
  const int fixedWidth =
      kBoardBraceWidth + scoreWidth + statusWidth + 3 * kBoardColumnGap;
  const int remainder = (rowRight - rowLeft) - fixedWidth;
  const int nameWidth = remainder > kBoardMinNameWidth ? remainder : kBoardMinNameWidth;

  const int scoreRight = nameLeft + nameWidth + scoreWidth;

  return BoardColumns{nameLeft, scoreRight, nameLeft,   scoreRight,
                      rowRight, nameWidth,  scoreWidth, statusWidth};
}

/// The one grid every two-line row of a board draws into, from the widest
/// status string on it. boardColumnsForBoard()'s counterpart, and the same two
/// calls under one name so the draw path has one thing to call.
constexpr BoardColumns boardStackedColumnsForBoard(int screenW, BoardScores scores,
                                                   int widestStatusTextWidth) {
  return boardStackedColumnsFor(
      screenW, scores,
      boardStackedStatusWidthFor(screenW, scores, widestStatusTextWidth));
}

constexpr int boardStackedSeparationFor(int screenW, BoardScores scores, int statusWidth) {
  const BoardColumns columns = boardStackedColumnsFor(screenW, scores, statusWidth);
  return (columns.statusRight - columns.statusWidth) - columns.homeScoreRight;
}

constexpr int boardStatusLeftFor(int screenW, BoardScores scores, int statusWidth) {
  const BoardColumns columns = boardColumnsFor(screenW, scores, statusWidth);
  return columns.statusRight - columns.statusWidth;
}

constexpr int boardStatusLeft(int screenW, BoardScores scores) {
  return boardStatusLeftFor(screenW, scores, boardDefaultStatusWidth(scores));
}

/// What stands between the home block's right edge and the status column's
/// left edge. Substituting `shared` back gives 2 * kBoardColumnGap for any
/// status width, score width and panel width, plus the one pixel the even split
/// cannot divide when the row's leftover is odd, which is why the two columns
/// cannot be made to touch by changing one of those figures.
constexpr int boardSeparationFor(int screenW, BoardScores scores, int statusWidth) {
  return boardStatusLeftFor(screenW, scores, statusWidth) -
         boardColumnsFor(screenW, scores, statusWidth).homeScoreRight;
}

constexpr int boardSeparation(int screenW, BoardScores scores) {
  return boardSeparationFor(screenW, scores, boardDefaultStatusWidth(scores));
}

/// The pixels the even split could not divide between the two names. They land
/// in the separation above, and whose parity they carry is the row's leftover,
/// not the panel width's: every status width in this file was even until one
/// was measured off a string. TEXT_LAYOUT_DESIGN.md section 15c.
///
/// `& 1` rather than `% 2` because the leftover goes negative on a panel too
/// narrow to hold the row, where `%` gives -1 and this gives 1. Neither answer
/// means anything there: the row is over budget and the kBoardMinNameWidth clamp
/// has already taken over, which is below 154px scores and 206px with scores
/// and under every panel the fleet has. The host test skips that region for the
/// same reason.
constexpr int boardLeftoverPixel(int screenW, BoardScores scores, int statusWidth) {
  return (boardRowSpan(screenW) -
          (2 * boardScoreWidth(scores) + statusWidth + 3 * kBoardColumnGap)) &
         1;
}

// The two panel widths on the fleet, checked by every build on both boards.
// They read the literals 320 and 240, so they speak for the two shipped panels
// and for no other width; the sweep in tests/card-columns-test.cpp is what
// covers a width nobody has built. TEXT_LAYOUT_DESIGN.md section 15 has the
// figures and TEST_PLAN.md section 18 the mutations they were proven against.
// They sit beside the arithmetic rather than in the host test, because an
// arduino-cli build of either board runs them and needs no host compiler.
static_assert(boardColumns(320, BoardScores::None).statusWidth >= kBoardStartTimeTextWidth,
              "a scoreless row's status holds a start time, and the widest is 78px");
static_assert(boardColumns(240, BoardScores::None).statusWidth >= kBoardStartTimeTextWidth,
              "a scoreless row's status holds a start time, and the widest is 78px");
static_assert(boardColumns(320, BoardScores::TwoDigit).statusWidth >= kBoardStateWordWidth,
              "a row with scores draws a state word there, and FINAL is 55px");
static_assert(boardColumns(240, BoardScores::TwoDigit).statusWidth >= kBoardStateWordWidth,
              "a row with scores draws a state word there, and FINAL is 55px");
static_assert(boardColumns(320, BoardScores::TwoDigit).nameWidth >= 81,
              "a row with scores keeps the 81px name column of section 15k");
static_assert(boardColumns(240, BoardScores::TwoDigit).nameWidth >= 41,
              "a row with scores keeps the 41px name column of section 15k");

// The two score columns against the digits drawn into them, which is what the
// pair of them was short of until section 15k. Equalities rather than `>=`: a
// column wider than the widest two digits is pixels taken off two name columns
// for nothing, and the four figures in 15k's own trade table are this 28.
static_assert(kBoardScoreWidth == kBoardWidestTwoDigitScoreWidth,
              "a one-line score column is the widest two digits of Sans12, which is 11 at 28");
static_assert(kBoardStackedScoreWidth == kBoardWidestTwoDigitStackedScoreWidth,
              "a two-line score column is the widest two digits of Sans9, which is any two at 20");
// Equalities here too. As `>=` these three are satisfied at every value the two
// equalities above permit, so flipping one to `<=` passed and they reported
// nothing about either panel - which is what the remark above
// boardColumnsForBoard()'s assertions calls an assertion reading its own
// definition back. An adversarial pass found all three.
static_assert(boardColumns(320, BoardScores::TwoDigit).scoreWidth == kBoardWidestTwoDigitScoreWidth,
              "the 320px panel's row holds a two-digit score with a 1 in it");
static_assert(boardColumns(240, BoardScores::TwoDigit).scoreWidth == kBoardWidestTwoDigitScoreWidth,
              "the 240px panel's row holds a two-digit score with a 1 in it");
static_assert(boardStackedScoreWidth(BoardScores::TwoDigit) == kBoardWidestTwoDigitStackedScoreWidth,
              "and a two-line row holds two digits of the face it draws them in");

// The three-digit state, as TEXT_LAYOUT_DESIGN.md section 15l writes it down.
// One column for both row shapes, because a three-digit board draws every score
// on it at Sans9 whichever shape the row is.
static_assert(boardScoreWidth(BoardScores::ThreeDigit) == kBoardThreeDigitScoreWidth,
              "a three-digit board's one-line score column holds three digits of Sans9");
static_assert(boardStackedScoreWidth(BoardScores::ThreeDigit) == kBoardThreeDigitScoreWidth,
              "and so does its two-line one, which is the same 30 at the same face");
static_assert(boardScoreWidth(BoardScores::None) == 0,
              "a board where no row reported a score draws no score column in either shape");
static_assert(boardStackedScoreWidth(BoardScores::None) == 0,
              "a board where no row reported a score draws no score column in either shape");

// The three states are three different answers, which is what says the enum is
// read rather than tested for one value. Without these a boardScoreWidth() that
// returned the two-digit column for both non-empty states would pass every
// assertion above.
static_assert(boardScoreWidth(BoardScores::ThreeDigit) > boardScoreWidth(BoardScores::TwoDigit),
              "a three-digit column is wider than a two-digit one on a one-line row");
static_assert(boardStackedScoreWidth(BoardScores::ThreeDigit) >
                  boardStackedScoreWidth(BoardScores::TwoDigit),
              "and on a two-line row, where it costs the one name the whole difference");

// Four digits are over the column, which is the gap 15l leaves open stated as a
// figure rather than as a sentence.
static_assert(kBoardWidestFourDigitScoreWidth > kBoardThreeDigitScoreWidth,
              "a four-digit score is over the column and draws an ellipsis");

// What the three-digit state costs a name, at both widths, on a board of state
// words. Two pixels on a one-line row because the two names split it, ten on a
// two-line row because the one name pays the lot.
static_assert(boardColumnsForBoard(320, BoardScores::ThreeDigit, kBoardStateWordWidth).nameWidth ==
                  79,
              "a 320px three-digit board of state words gives each name 79px, down from 81");
static_assert(boardColumnsForBoard(240, BoardScores::ThreeDigit, kBoardStateWordWidth).nameWidth ==
                  39,
              "a 240px three-digit board of state words gives each name 39px, down from 41");
static_assert(boardStackedColumnsForBoard(240, BoardScores::ThreeDigit, kBoardStateWordWidth)
                      .nameWidth == 100,
              "a 240px three-digit two-line board of state words gives 100px, down from 110");
static_assert(boardStackedColumnsForBoard(240, BoardScores::ThreeDigit,
                                          kBoardShortStartTimeTextWidth)
                      .nameWidth == 87,
              "and the photographed board's status takes it to 87, down from 97");

// The edges do not move with the score column, in any of the three states. The
// name and the score column sum to the same figure, so a wider score column
// takes the pixels from the name beside it and from nothing else. This is the
// identity 15l states, read at the one width where all three are interesting.
static_assert(boardColumnsForBoard(320, BoardScores::ThreeDigit, kBoardStateWordWidth)
                      .homeNameLeft ==
                  boardColumnsForBoard(320, BoardScores::TwoDigit, kBoardStateWordWidth)
                      .homeNameLeft,
              "a three-digit board starts its home name where a two-digit board does");
static_assert(boardColumnsForBoard(320, BoardScores::ThreeDigit, kBoardStateWordWidth)
                      .homeScoreRight ==
                  boardColumnsForBoard(320, BoardScores::TwoDigit, kBoardStateWordWidth)
                      .homeScoreRight,
              "and ends its home score there too");
static_assert(boardColumnsForBoard(320, BoardScores::None, kBoardStateWordWidth).homeNameLeft ==
                  boardColumnsForBoard(320, BoardScores::TwoDigit, kBoardStateWordWidth)
                      .homeNameLeft,
              "and so does a board that reported no score at all");

// The three-digit ceilings, as the figures 15l writes down.
static_assert(boardMaxStatusWidth(320, BoardScores::ThreeDigit) == 156,
              "320 three-digit can spend 156 on a status");
static_assert(boardMaxStatusWidth(240, BoardScores::ThreeDigit) == 76,
              "240 three-digit can spend 76");
static_assert(boardStackedMaxStatusWidth(320, BoardScores::ThreeDigit) == 207,
              "320 three-digit two-line, 207");
static_assert(boardStackedMaxStatusWidth(240, BoardScores::ThreeDigit) == 127,
              "240 three-digit two-line, 127");

// And at each of those the name stands exactly on the floor, which is the
// tightest property in this file and the one that says the ceiling is derived
// from the floor rather than picked.
static_assert(boardColumnsFor(320, BoardScores::ThreeDigit,
                              boardMaxStatusWidth(320, BoardScores::ThreeDigit))
                      .nameWidth == kBoardNameFloor,
              "320 three-digit holds the name floor at its ceiling");
static_assert(boardColumnsFor(240, BoardScores::ThreeDigit,
                              boardMaxStatusWidth(240, BoardScores::ThreeDigit))
                      .nameWidth == kBoardNameFloor,
              "240 three-digit holds the name floor at its ceiling");
static_assert(boardStackedColumnsFor(240, BoardScores::ThreeDigit,
                                     boardStackedMaxStatusWidth(240, BoardScores::ThreeDigit))
                      .nameWidth == kBoardNameFloor,
              "240 three-digit two-line holds the name floor at its ceiling");

// The one property the three-digit state loses, asserted as the loss it is so
// closing it has to come through this line and section 15l. A 240px one-line
// board with three-digit scores cuts the widest start time by 2px. The 240px
// panel on the fleet is portrait and draws the two-line form, so no board has
// this shape; the two-digit assertion above is the one the fleet reads.
static_assert(boardStatusWidthFor(240, BoardScores::ThreeDigit, kBoardStartTimeTextWidth) <
                  kBoardStartTimeTextWidth,
              "a 240px three-digit one-line row cannot hold the widest start time");
static_assert(boardStatusWidthFor(320, BoardScores::ThreeDigit, kBoardStartTimeTextWidth) >=
                  kBoardStartTimeTextWidth,
              "and a 320px one does, which is every landscape board the fleet has");
static_assert(boardStackedStatusWidthFor(240, BoardScores::ThreeDigit, kBoardStartTimeTextWidth) >=
                  kBoardStartTimeTextWidth,
              "and so does the two-line row, which is what the 240px panel draws");

// The separation holds only while the kBoardMinNameWidth floor is not engaged:
// the floor clamps the name width and nothing clamps the edges derived from it,
// so below 174px scores and 206px with scores the separation falls under
// 2 * kBoardColumnGap, and below 162 and 194 the home block and the status
// column overlap. All four are under every panel the fleet has, and these two
// pairs are what says so at the two it has. The two "with scores" figures were
// 202 and 190 at the 26px score column section 15k corrected.
static_assert(boardColumns(320, BoardScores::TwoDigit).nameWidth > kBoardMinNameWidth, "320 clears the floor");
static_assert(boardColumns(240, BoardScores::TwoDigit).nameWidth > kBoardMinNameWidth, "240 clears the floor");
static_assert(boardSeparation(320, BoardScores::None) >= 2 * kBoardColumnGap, "320 scoreless stays apart");
static_assert(boardSeparation(320, BoardScores::TwoDigit) >= 2 * kBoardColumnGap, "320 scored stays apart");
static_assert(boardSeparation(240, BoardScores::None) >= 2 * kBoardColumnGap, "240 scoreless stays apart");
static_assert(boardSeparation(240, BoardScores::TwoDigit) >= 2 * kBoardColumnGap, "240 scored stays apart");

// Right edge inside the content band at both widths, which is where the row of
// literals this replaced put 248 on a 240px panel.
static_assert(boardColumns(320, BoardScores::None).statusRight <= 320 - kCardMargin, "320 is on the glass");
static_assert(boardColumns(240, BoardScores::None).statusRight <= 240 - kCardMargin, "240 is on the glass");

// The measured column, as the two properties TEXT_LAYOUT_DESIGN.md sections 15g
// and 15f are each about. Both read boardStatusWidthFor() at a figure measured
// off the glyph table, which is the whole claim: the column holds the string it
// was given.
//
// Equalities rather than `>=`. A `>=` is satisfied at 320 and 240 scoreless by
// the 80px the shipped layout already handed those rows, so four assertions
// would have read as four properties and bitten on two. 74 plus the slack is 76
// at every one of the four, and the shipped 80 is not 76.
static_assert(boardStatusWidthFor(320, BoardScores::TwoDigit, kBoardPeriodTextWidth) ==
                  kBoardPeriodTextWidth + kBoardStatusSlack,
              "a live row with scores draws a period, and 6TH INN is 74px");
static_assert(boardStatusWidthFor(240, BoardScores::TwoDigit, kBoardPeriodTextWidth) ==
                  kBoardPeriodTextWidth + kBoardStatusSlack,
              "a live row with scores draws a period, and 6TH INN is 74px");
static_assert(boardStatusWidthFor(320, BoardScores::None, kBoardPeriodTextWidth) ==
                  kBoardPeriodTextWidth + kBoardStatusSlack,
              "a live row with no score sent asks for the period it holds, not the state's 80");
static_assert(boardStatusWidthFor(240, BoardScores::None, kBoardPeriodTextWidth) ==
                  kBoardPeriodTextWidth + kBoardStatusSlack,
              "a live row with no score sent asks for the period it holds, not the state's 80");
static_assert(boardStatusWidthFor(320, BoardScores::TwoDigit, kBoardStartTimeTextWidth) >= kBoardStartTimeTextWidth,
              "a scheduled row that reported one score still holds a start time");
static_assert(boardStatusWidthFor(240, BoardScores::TwoDigit, kBoardStartTimeTextWidth) >= kBoardStartTimeTextWidth,
              "a scheduled row that reported one score still holds a start time");

// The same two columns as literals, because the four equalities above carry
// kBoardStatusSlack on both sides and move with it. Written as 76 and 80, a
// slack of 0 or of 3 fails here; written as the sum, it passes every assertion
// in this file and only the host run reads it.
static_assert(boardStatusWidthFor(320, BoardScores::TwoDigit, kBoardPeriodTextWidth) == 76,
              "6TH INN gets 76px, which is its 74 and the 2px of slack");
static_assert(boardStatusWidthFor(320, BoardScores::TwoDigit, kBoardStartTimeTextWidth) == 80,
              "the widest start time gets 80px on a row with scores");

// And the other half of the trade: a state word that already fits leaves the
// name columns exactly where v2026.10.31.0002 put them. kBoardStateWordWidth is
// FINAL's 55; PPD's 37 is in the host test's table rather than here, because a
// second figure for the same property is a second figure to keep.
static_assert(boardColumnsFor(320, BoardScores::TwoDigit, boardStatusWidthFor(320, BoardScores::TwoDigit, kBoardStateWordWidth))
                      .nameWidth == 81,
              "FINAL costs the 320px panel's names no pixels");
static_assert(boardColumnsFor(240, BoardScores::TwoDigit, boardStatusWidthFor(240, BoardScores::TwoDigit, kBoardStateWordWidth))
                      .nameWidth == 41,
              "FINAL costs the 240px panel's names no pixels");

// The four ceilings, as the figures section 15h writes down rather than as
// `>= kBoardStatusWidth`, which boardMaxStatusWidth()'s own ternary makes true
// for every input and so reports nothing about a panel width.
static_assert(boardMaxStatusWidth(320, BoardScores::None) == 216, "320 scoreless can spend 216 on a status");
static_assert(boardMaxStatusWidth(320, BoardScores::TwoDigit) == 160, "320 with scores can spend 160");
static_assert(boardMaxStatusWidth(240, BoardScores::None) == 136, "240 scoreless can spend 136");
static_assert(boardMaxStatusWidth(240, BoardScores::TwoDigit) == 80, "240 with scores can spend 80");

// And at the ceiling both name columns stand exactly on the floor it was
// derived from, which is the tightest of these: a pixel either way fails.
static_assert(boardColumnsFor(320, BoardScores::TwoDigit, boardMaxStatusWidth(320, BoardScores::TwoDigit)).nameWidth ==
                  kBoardNameFloor,
              "320 with scores holds the name floor at its ceiling");
static_assert(boardColumnsFor(240, BoardScores::TwoDigit, boardMaxStatusWidth(240, BoardScores::TwoDigit)).nameWidth ==
                  kBoardNameFloor,
              "240 with scores holds the name floor at its ceiling");
static_assert(boardColumnsFor(320, BoardScores::None, boardMaxStatusWidth(320, BoardScores::None)).nameWidth ==
                  kBoardNameFloor,
              "320 scoreless holds the name floor at its ceiling");
static_assert(boardColumnsFor(240, BoardScores::None, boardMaxStatusWidth(240, BoardScores::None)).nameWidth ==
                  kBoardNameFloor,
              "240 scoreless holds the name floor at its ceiling");

// The board's own grid, as the figures TEXT_LAYOUT_DESIGN.md section 15i writes
// down. Each one is the grid a whole board lands on when its widest status
// measures what the argument says, which is what every row of it then draws
// into.
//
// Literals on the right rather than the same call written twice.
// boardColumnsForBoard() is boardColumnsFor() over boardStatusWidthFor(), so an
// assertion built from those two reads its own definition back and reports
// nothing about either figure.
static_assert(boardColumnsForBoard(320, BoardScores::TwoDigit, kBoardWidestPeriodTextWidth).nameWidth == 61,
              "a 320px board holding a HALFTIME gives all four rows 61px names");
static_assert(boardColumnsForBoard(320, BoardScores::TwoDigit, kBoardWidestPeriodTextWidth).homeNameLeft == 109,
              "and starts every home name at 109, which the wider score column does not move");
static_assert(boardColumnsForBoard(240, BoardScores::TwoDigit, kBoardWidestPeriodTextWidth).nameWidth ==
                  kBoardNameFloor,
              "a 240px board holding a HALFTIME stands every row on the name floor");
static_assert(boardColumnsForBoard(320, BoardScores::TwoDigit, kBoardStartTimeTextWidth).nameWidth == 69,
              "a 320px board holding the widest start time gives 69px names");
static_assert(boardColumnsForBoard(240, BoardScores::TwoDigit, kBoardStartTimeTextWidth).nameWidth ==
                  kBoardNameFloor,
              "a 240px board holding the widest start time stands on the floor too");

// A board of state words costs its names nothing, which is the other half of
// the trade: the figures v2026.10.31.0002 shipped stand on every row of it.
static_assert(boardColumnsForBoard(320, BoardScores::TwoDigit, kBoardStateWordWidth).nameWidth == 81,
              "four FINALs on a 320px board draw an 81px name column");
static_assert(boardColumnsForBoard(240, BoardScores::TwoDigit, kBoardStateWordWidth).nameWidth == 41,
              "four FINALs on a 240px board draw a 41px name column");

// A board where no row reported a score releases both score columns, on every
// row of it. The board's scores, not a row's.
static_assert(boardColumnsForBoard(320, BoardScores::None, kBoardStartTimeTextWidth).scoreWidth == 0,
              "a board of scheduled games draws no score column");
static_assert(boardColumnsForBoard(320, BoardScores::TwoDigit, kBoardStartTimeTextWidth).scoreWidth ==
                  kBoardScoreWidth,
              "one game under way gives every row of the board a score column");

// The two-line row, as the figures TEXT_LAYOUT_DESIGN.md section 15j writes
// down. 240 is the profile panel it was written for; 320 is what a profile
// panel of that width would draw, which no board on the fleet has, and the
// host sweep is what covers every width between and beyond.
//
// The photographed board first. Four NHL games reading FINAL, FINAL, P3 and a
// 6:00 PM start time, so the widest status on it is the start time's 67px and
// the column is 69. Literals on the right, because an assertion built from the
// same two calls reads its own definition back.
static_assert(boardStackedColumnsForBoard(240, BoardScores::TwoDigit, kBoardShortStartTimeTextWidth).nameWidth ==
                  97,
              "the photographed board's 67px start time gives a two-line row a 97px name");
static_assert(boardColumnsForBoard(240, BoardScores::TwoDigit, kBoardShortStartTimeTextWidth).nameWidth == 34,
              "where the one-line row on the same board draws 34");

// Hurricanes is 94px of FreeSansBold9pt7b and Mammoth 86, the two names the
// photograph cut to Hur... and Ma..., and 97 holds both whole.
static_assert(boardStackedColumnsForBoard(240, BoardScores::TwoDigit, kBoardShortStartTimeTextWidth).nameWidth >=
                  94,
              "Hurricanes measures 94 and the two-line row spells it");

// And a board of state words, where the status costs the name nothing.
static_assert(boardStackedColumnsForBoard(240, BoardScores::TwoDigit, kBoardStateWordWidth).nameWidth == 110,
              "a 240px board of state words gives a two-line row a 110px name");
static_assert(boardStackedColumnsForBoard(240, BoardScores::TwoDigit, kBoardStateWordWidth).statusWidth ==
                  kBoardStatusWidth,
              "and FINAL costs that name no pixels, the same trade the one-line row makes");
static_assert(boardColumnsForBoard(240, BoardScores::TwoDigit, kBoardStateWordWidth).nameWidth == 41,
              "which is the 41px the one-line row draws on a board of state words");

// The one string 15h and 15i both record the profile board as unable to hold.
// One kBoardNameFloor in the ceiling instead of two is the whole of the fix.
static_assert(boardStackedMaxStatusWidth(240, BoardScores::TwoDigit) == 137, "240 two-line can spend 137");
static_assert(boardStackedMaxStatusWidth(240, BoardScores::None) == 157, "240 two-line scores, 157");
static_assert(boardStackedMaxStatusWidth(320, BoardScores::TwoDigit) == 217, "320 two-line can spend 217");
static_assert(boardStackedMaxStatusWidth(320, BoardScores::None) == 237, "320 two-line scores, 237");
static_assert(boardStackedColumnsForBoard(240, BoardScores::TwoDigit, kBoardWidestPeriodTextWidth).statusWidth ==
                  kBoardWidestPeriodTextWidth + kBoardStatusSlack,
              "HALFTIME gets the 96px column it asks for on a 240px two-line board");
static_assert(boardStackedColumnsForBoard(240, BoardScores::TwoDigit, kBoardWidestPeriodTextWidth).nameWidth == 70,
              "and the name that buys it is 70, not the floor");
static_assert(boardColumnsForBoard(240, BoardScores::TwoDigit, kBoardWidestPeriodTextWidth).statusWidth <
                  kBoardWidestPeriodTextWidth,
              "where the one-line row on the same panel still cuts it");

// A board where no row reported a score releases the one score column, and a
// board with nothing in its status column spends the lot on the name.
static_assert(boardStackedColumnsForBoard(240, BoardScores::None, kBoardStartTimeTextWidth).scoreWidth == 0,
              "a two-line board of scheduled games draws no score column");
static_assert(boardStackedColumnsForBoard(240, BoardScores::None, kBoardStartTimeTextWidth).nameWidth == 106,
              "and gives its name 106px at the widest start time");
static_assert(boardStackedColumnsForBoard(240, BoardScores::TwoDigit, kBoardStartTimeTextWidth).scoreWidth ==
                  kBoardStackedScoreWidth,
              "one game under way gives every two-line row a score column");
static_assert(boardStackedColumnsForBoard(240, BoardScores::TwoDigit, 0).statusWidth == 0,
              "a board whose rows sent no status draws no status column");
static_assert(boardStackedColumnsForBoard(240, BoardScores::TwoDigit, 0).nameWidth == 166,
              "and spends the lot on the name");

// The two lines sit at one x and differ in y, which is what lets one set of
// draw calls serve both orientations.
static_assert(boardStackedColumnsFor(240, BoardScores::TwoDigit, kBoardStatusWidth).awayNameLeft ==
                  boardStackedColumnsFor(240, BoardScores::TwoDigit, kBoardStatusWidth).homeNameLeft,
              "both lines of a two-line row start their name at one edge");
static_assert(boardStackedColumnsFor(240, BoardScores::TwoDigit, kBoardStatusWidth).awayScoreRight ==
                  boardStackedColumnsFor(240, BoardScores::TwoDigit, kBoardStatusWidth).homeScoreRight,
              "and end their score at one edge");

// The brace stands inside the content band with the name clear of it, and the
// row still ends on the glass.
static_assert(boardStackedBraceLeft() >= kCardMargin,
              "the brace stands inside the content band, not in the margin the chevrons use");
static_assert(boardStackedColumnsFor(240, BoardScores::TwoDigit, kBoardStatusWidth).awayNameLeft ==
                  boardStackedBraceLeft() + kBoardBraceWidth + kBoardColumnGap,
              "and a column gap stands between the brace and the name");
static_assert(boardStackedColumnsFor(240, BoardScores::TwoDigit, kBoardStatusWidth).statusRight <=
                  240 - kCardMargin,
              "240 two-line is on the glass");
static_assert(boardStackedColumnsFor(320, BoardScores::TwoDigit, kBoardStatusWidth).statusRight <=
                  320 - kCardMargin,
              "320 two-line is on the glass");

// The separation is exactly two gaps and carries no parity term, because one
// name takes the whole remainder and nothing is split in half.
static_assert(boardStackedSeparationFor(240, BoardScores::TwoDigit, kBoardStatusWidth) == 2 * kBoardColumnGap,
              "240 two-line with scores keeps two gaps of air");
static_assert(boardStackedSeparationFor(240, BoardScores::None, kBoardStartTimeWidth) == 2 * kBoardColumnGap,
              "240 two-line scores keeps two gaps of air");
static_assert(boardStackedSeparationFor(241, BoardScores::TwoDigit, kBoardStatusWidth) == 2 * kBoardColumnGap,
              "and an odd panel width keeps two gaps as well, where the one-line row keeps 13");

// At the ceiling the one name column stands exactly on the floor it was
// derived from, which is the tightest of these: a pixel either way fails.
static_assert(boardStackedColumnsFor(240, BoardScores::TwoDigit, boardStackedMaxStatusWidth(240, BoardScores::TwoDigit))
                      .nameWidth == kBoardNameFloor,
              "240 two-line with scores holds the name floor at its ceiling");
static_assert(boardStackedColumnsFor(240, BoardScores::None,
                                     boardStackedMaxStatusWidth(240, BoardScores::None))
                      .nameWidth == kBoardNameFloor,
              "240 two-line scoreless holds the name floor at its ceiling");

}  // namespace CardColumns
