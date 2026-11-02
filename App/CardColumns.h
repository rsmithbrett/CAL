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
///
/// The status column's width comes from the string it holds. Section 15h is the
/// shape and section 15g is what the fixed width cut. The board gives its
/// status column the width its widest string asks for and lays every row on
/// the one grid that leaves, which is section 15i. That grid is not always one
/// of the grids its rows would have asked for on their own, because `scoreless`
/// is the board's question and not a row's.
///
/// Nothing here reads App/Display.cpp, so the arguments the draw path hands
/// these functions are checked by ci/check-board-columns-call.sh instead, a
/// text gate over showSportsBoard() and the fold that feeds it. TEST_PLAN.md
/// sections 18e and 18f have the single-token changes to those lines that each
/// drew a wrong board with nothing in this file firing.
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

// A score column holds two digits. kBoardStatusWidth is the narrowest status
// column the row ever draws and the floor boardStatusWidthFor() starts from;
// what it holds is a state word, a period or a start time, and those are not
// the same width, so the width comes from the string rather than from here.
// kBoardMinNameWidth is the clamp inside boardColumns(), below which the edges
// derived from the name width stop holding - a different figure from
// kBoardNameFloor, which is what the status column's ceiling is budgeted
// against.
constexpr int kBoardScoreWidth = 26;
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
/// figure here that is one. It sits 7px clear of kBoardMinNameWidth, where the
/// clamp below engages and the separation identity stops holding, and it is the
/// largest floor that still leaves the 240px panel room to hand a start time
/// the 80px a start time measures. TEXT_LAYOUT_DESIGN.md section 15h has what
/// it costs each name column at each panel width.
constexpr int kBoardNameFloor = 31;

/// How far in from each edge of the panel the first and last column edge sit, on
/// top of kCardMargin.
constexpr int kBoardRowInset = 4;

/// The two score columns a row gets, which is where the status column's pixels
/// come from.
///
/// `scoreless` means both score strings are empty, which is not the same thing
/// as "the status holds a start time": Sports.cpp writes each side
/// independently, so a scheduled game with one score reported is not scoreless.
/// That is why the status width is measured rather than taken from this.
/// TEXT_LAYOUT_DESIGN.md sections 15f and 15h.
constexpr int boardScoreWidth(bool scoreless) { return scoreless ? 0 : kBoardScoreWidth; }

/// The pixels between the row's first column edge and its last.
constexpr int boardRowSpan(int screenW) {
  return (screenW - kCardMargin - kBoardRowInset) - (kCardMargin + kBoardRowInset);
}

/// The status width a row's state asks for before anything measures its string.
/// What boardColumns() below uses, and what the fourteen assertions predating
/// section 15h read.
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
  const int want = measuredTextWidth + kBoardStatusSlack;
  const int ceiling = boardMaxStatusWidth(screenW, scoreless);
  return want < ceiling ? want : ceiling;
}

/// Three blocks, all from one figure: whatever the row has left after the score
/// and status columns splits evenly between the two names. Every edge is derived
/// from the panel width, so a 240px panel lays out the same way a 320px one does
/// instead of inheriting literals that held at one width.
///
/// Two inputs beyond the panel width. `scoreless` picks the score columns, which
/// is where the pixels come from: two empty score columns release what the
/// status column spends. `statusWidth` is what the status column takes, and the
/// caller has to pass the same figure it draws into - boardStatusWidthFor()
/// above is what produces it from the string.
///
/// constexpr so the properties below are checked by every build on both panels.
constexpr BoardColumns boardColumnsFor(int screenW, bool scoreless, int statusWidth) {
  const int rowLeft = kCardMargin + kBoardRowInset;
  const int rowRight = screenW - kCardMargin - kBoardRowInset;

  const int scoreWidth = boardScoreWidth(scoreless);

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
/// measurement and return the scoreless layout - and every assertion in this
/// file and every case in the host test would pass, because none of them reads
/// the draw path. Two arities under one name is the shape that allows that;
/// two names do not.
constexpr BoardColumns boardColumns(int screenW, bool scoreless) {
  return boardColumnsFor(screenW, scoreless, boardDefaultStatusWidth(scoreless));
}

/// The one grid every row of a board draws into.
///
/// `scoreless` is the board's rather than a row's: true only when no row of it
/// reported either score. One game under way gives all four rows a score
/// column, and a row with no score of its own draws nothing into it.
///
/// `widestStatusTextWidth` is the widest any of the board's statuses measures,
/// not the widest column any of them asks for. boardStatusWidthFor() does not
/// fall as its measurement rises, so those two are the same column and the
/// caller can fold the measurements and convert once. The host sweep asserts
/// that rather than leaving it as the reason this remark gives.
///
/// TEXT_LAYOUT_DESIGN.md section 15i. 15h's table is what each row would have
/// asked for alone; the board picks the widest status of them, and then
/// re-derives the rest from its own `scoreless`, so the grid it lands on need
/// not be any row's.
constexpr BoardColumns boardColumnsForBoard(int screenW, bool scoreless,
                                            int widestStatusTextWidth) {
  return boardColumnsFor(screenW, scoreless,
                         boardStatusWidthFor(screenW, scoreless, widestStatusTextWidth));
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
/// describes. kBoardScoreWidth's 26 is the same two digits at Sans12, which is
/// the face a one-line row draws them in and a two-line row has no height for:
/// two Sans12 slots are 58px against the 52px card the profile panel gives a
/// row. Section 15j is the trade.
constexpr int kBoardBraceWidth = 8;
constexpr int kBoardStackedScoreWidth = 20;

/// The brace's left edge, which is the row's own left edge.
constexpr int boardStackedBraceLeft() { return kCardMargin + kBoardRowInset; }

/// The one score column a two-line row draws, which both of its lines share.
/// Released by the board's `scoreless` on the same rule boardScoreWidth() uses.
constexpr int boardStackedScoreWidth(bool scoreless) {
  return scoreless ? 0 : kBoardStackedScoreWidth;
}

/// The widest status column a two-line row can be given.
///
/// boardMaxStatusWidth()'s shape with one kBoardNameFloor subtracted instead of
/// two, because a two-line row has one name column where a one-line row has
/// two. That single subtraction is what takes HALFTIME off the list of strings
/// the 240px panel cannot hold: 94px asks for 96, the one-line ceiling there is
/// 80, and this one is 135.
constexpr int boardStackedMaxStatusWidth(int screenW, bool scoreless) {
  const int fromName = boardRowSpan(screenW) - kBoardBraceWidth -
                       boardStackedScoreWidth(scoreless) - 3 * kBoardColumnGap -
                       kBoardNameFloor;
  return fromName > kBoardStatusWidth ? fromName : kBoardStatusWidth;
}

/// The three rules boardStatusWidthFor() states, under the two-line ceiling.
/// Nothing is drawn for an empty status, a status that fits the 56px column
/// keeps it, and anything wider asks for its own width plus the slack.
constexpr int boardStackedStatusWidthFor(int screenW, bool scoreless,
                                         int measuredTextWidth) {
  if (measuredTextWidth <= 0) { return 0; }
  if (measuredTextWidth <= kBoardStatusWidth) { return kBoardStatusWidth; }
  const int want = measuredTextWidth + kBoardStatusSlack;
  const int ceiling = boardStackedMaxStatusWidth(screenW, scoreless);
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
constexpr BoardColumns boardStackedColumnsFor(int screenW, bool scoreless,
                                              int statusWidth) {
  const int rowLeft = boardStackedBraceLeft();
  const int rowRight = screenW - kCardMargin - kBoardRowInset;
  const int scoreWidth = boardStackedScoreWidth(scoreless);
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
constexpr BoardColumns boardStackedColumnsForBoard(int screenW, bool scoreless,
                                                   int widestStatusTextWidth) {
  return boardStackedColumnsFor(
      screenW, scoreless,
      boardStackedStatusWidthFor(screenW, scoreless, widestStatusTextWidth));
}

constexpr int boardStackedSeparationFor(int screenW, bool scoreless, int statusWidth) {
  const BoardColumns columns = boardStackedColumnsFor(screenW, scoreless, statusWidth);
  return (columns.statusRight - columns.statusWidth) - columns.homeScoreRight;
}

constexpr int boardStatusLeftFor(int screenW, bool scoreless, int statusWidth) {
  const BoardColumns columns = boardColumnsFor(screenW, scoreless, statusWidth);
  return columns.statusRight - columns.statusWidth;
}

constexpr int boardStatusLeft(int screenW, bool scoreless) {
  return boardStatusLeftFor(screenW, scoreless, boardDefaultStatusWidth(scoreless));
}

/// What stands between the home block's right edge and the status column's
/// left edge. Substituting `shared` back gives 2 * kBoardColumnGap for any
/// status width, score width and panel width, plus the one pixel the even split
/// cannot divide when the row's leftover is odd, which is why the two columns
/// cannot be made to touch by changing one of those figures.
constexpr int boardSeparationFor(int screenW, bool scoreless, int statusWidth) {
  return boardStatusLeftFor(screenW, scoreless, statusWidth) -
         boardColumnsFor(screenW, scoreless, statusWidth).homeScoreRight;
}

constexpr int boardSeparation(int screenW, bool scoreless) {
  return boardSeparationFor(screenW, scoreless, boardDefaultStatusWidth(scoreless));
}

/// The pixels the even split could not divide between the two names. They land
/// in the separation above, and whose parity they carry is the row's leftover,
/// not the panel width's: every status width in this file was even until one
/// was measured off a string. TEXT_LAYOUT_DESIGN.md section 15c.
///
/// `& 1` rather than `% 2` because the leftover goes negative on a panel too
/// narrow to hold the row, where `%` gives -1 and this gives 1. Neither answer
/// means anything there: the row is over budget and the kBoardMinNameWidth clamp
/// has already taken over, which is below 154px scoreless and 206px with scores
/// and under every panel the fleet has. The host test skips that region for the
/// same reason.
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
//
// Equalities rather than `>=`. A `>=` is satisfied at 320 and 240 scoreless by
// the 80px the shipped layout already handed those rows, so four assertions
// would have read as four properties and bitten on two. 74 plus the slack is 76
// at every one of the four, and the shipped 80 is not 76.
static_assert(boardStatusWidthFor(320, false, kBoardPeriodTextWidth) ==
                  kBoardPeriodTextWidth + kBoardStatusSlack,
              "a live row with scores draws a period, and 6TH INN is 74px");
static_assert(boardStatusWidthFor(240, false, kBoardPeriodTextWidth) ==
                  kBoardPeriodTextWidth + kBoardStatusSlack,
              "a live row with scores draws a period, and 6TH INN is 74px");
static_assert(boardStatusWidthFor(320, true, kBoardPeriodTextWidth) ==
                  kBoardPeriodTextWidth + kBoardStatusSlack,
              "a live row with no score sent asks for the period it holds, not the state's 80");
static_assert(boardStatusWidthFor(240, true, kBoardPeriodTextWidth) ==
                  kBoardPeriodTextWidth + kBoardStatusSlack,
              "a live row with no score sent asks for the period it holds, not the state's 80");
static_assert(boardStatusWidthFor(320, false, kBoardStartTimeTextWidth) >= kBoardStartTimeTextWidth,
              "a scheduled row that reported one score still holds a start time");
static_assert(boardStatusWidthFor(240, false, kBoardStartTimeTextWidth) >= kBoardStartTimeTextWidth,
              "a scheduled row that reported one score still holds a start time");

// The same two columns as literals, because the four equalities above carry
// kBoardStatusSlack on both sides and move with it. Written as 76 and 80, a
// slack of 0 or of 3 fails here; written as the sum, it passes every assertion
// in this file and only the host run reads it.
static_assert(boardStatusWidthFor(320, false, kBoardPeriodTextWidth) == 76,
              "6TH INN gets 76px, which is its 74 and the 2px of slack");
static_assert(boardStatusWidthFor(320, false, kBoardStartTimeTextWidth) == 80,
              "the widest start time gets 80px on a row with scores");

// And the other half of the trade: a state word that already fits leaves the
// name columns exactly where v2026.10.31.0002 put them. kBoardStateWordWidth is
// FINAL's 55; PPD's 37 is in the host test's table rather than here, because a
// second figure for the same property is a second figure to keep.
static_assert(boardColumnsFor(320, false, boardStatusWidthFor(320, false, kBoardStateWordWidth))
                      .nameWidth == 83,
              "FINAL costs the 320px panel's names no pixels");
static_assert(boardColumnsFor(240, false, boardStatusWidthFor(240, false, kBoardStateWordWidth))
                      .nameWidth == 43,
              "FINAL costs the 240px panel's names no pixels");

// The four ceilings, as the figures section 15h writes down rather than as
// `>= kBoardStatusWidth`, which boardMaxStatusWidth()'s own ternary makes true
// for every input and so reports nothing about a panel width.
static_assert(boardMaxStatusWidth(320, true) == 212, "320 scoreless can spend 212 on a status");
static_assert(boardMaxStatusWidth(320, false) == 160, "320 with scores can spend 160");
static_assert(boardMaxStatusWidth(240, true) == 132, "240 scoreless can spend 132");
static_assert(boardMaxStatusWidth(240, false) == 80, "240 with scores can spend 80");

// And at the ceiling both name columns stand exactly on the floor it was
// derived from, which is the tightest of these: a pixel either way fails.
static_assert(boardColumnsFor(320, false, boardMaxStatusWidth(320, false)).nameWidth ==
                  kBoardNameFloor,
              "320 with scores holds the name floor at its ceiling");
static_assert(boardColumnsFor(240, false, boardMaxStatusWidth(240, false)).nameWidth ==
                  kBoardNameFloor,
              "240 with scores holds the name floor at its ceiling");
static_assert(boardColumnsFor(320, true, boardMaxStatusWidth(320, true)).nameWidth ==
                  kBoardNameFloor,
              "320 scoreless holds the name floor at its ceiling");
static_assert(boardColumnsFor(240, true, boardMaxStatusWidth(240, true)).nameWidth ==
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
static_assert(boardColumnsForBoard(320, false, kBoardWidestPeriodTextWidth).nameWidth == 63,
              "a 320px board holding a HALFTIME gives all four rows 63px names");
static_assert(boardColumnsForBoard(320, false, kBoardWidestPeriodTextWidth).homeNameLeft == 109,
              "and starts every home name at 109");
static_assert(boardColumnsForBoard(240, false, kBoardWidestPeriodTextWidth).nameWidth ==
                  kBoardNameFloor,
              "a 240px board holding a HALFTIME stands every row on the name floor");
static_assert(boardColumnsForBoard(320, false, kBoardStartTimeTextWidth).nameWidth == 71,
              "a 320px board holding the widest start time gives 71px names");
static_assert(boardColumnsForBoard(240, false, kBoardStartTimeTextWidth).nameWidth ==
                  kBoardNameFloor,
              "a 240px board holding the widest start time stands on the floor too");

// A board of state words costs its names nothing, which is the other half of
// the trade: the figures v2026.10.31.0002 shipped stand on every row of it.
static_assert(boardColumnsForBoard(320, false, kBoardStateWordWidth).nameWidth == 83,
              "four FINALs on a 320px board draw the 83px name column the fleet draws");
static_assert(boardColumnsForBoard(240, false, kBoardStateWordWidth).nameWidth == 43,
              "four FINALs on a 240px board draw the 43px name column the fleet draws");

// A board where no row reported a score releases both score columns, on every
// row of it. The board's scoreless, not a row's.
static_assert(boardColumnsForBoard(320, true, kBoardStartTimeTextWidth).scoreWidth == 0,
              "a board of scheduled games draws no score column");
static_assert(boardColumnsForBoard(320, false, kBoardStartTimeTextWidth).scoreWidth ==
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
static_assert(boardStackedColumnsForBoard(240, false, kBoardShortStartTimeTextWidth).nameWidth ==
                  97,
              "the photographed board's 67px start time gives a two-line row a 97px name");
static_assert(boardColumnsForBoard(240, false, kBoardShortStartTimeTextWidth).nameWidth == 36,
              "where the one-line row on the same board draws 36");

// Hurricanes is 94px of FreeSansBold9pt7b and Mammoth 86, the two names the
// photograph cut to Hur... and Ma..., and 97 holds both whole.
static_assert(boardStackedColumnsForBoard(240, false, kBoardShortStartTimeTextWidth).nameWidth >=
                  94,
              "Hurricanes measures 94 and the two-line row spells it");

// And a board of state words, where the status costs the name nothing.
static_assert(boardStackedColumnsForBoard(240, false, kBoardStateWordWidth).nameWidth == 110,
              "a 240px board of state words gives a two-line row a 110px name");
static_assert(boardStackedColumnsForBoard(240, false, kBoardStateWordWidth).statusWidth ==
                  kBoardStatusWidth,
              "and FINAL costs that name no pixels, the same trade the one-line row makes");
static_assert(boardColumnsForBoard(240, false, kBoardStateWordWidth).nameWidth == 43,
              "which is the 43px the one-line row draws on a board of state words");

// The one string 15h and 15i both record the profile board as unable to hold.
// One kBoardNameFloor in the ceiling instead of two is the whole of the fix.
static_assert(boardStackedMaxStatusWidth(240, false) == 135, "240 two-line can spend 135");
static_assert(boardStackedMaxStatusWidth(240, true) == 155, "240 two-line scoreless, 155");
static_assert(boardStackedMaxStatusWidth(320, false) == 215, "320 two-line can spend 215");
static_assert(boardStackedMaxStatusWidth(320, true) == 235, "320 two-line scoreless, 235");
static_assert(boardStackedColumnsForBoard(240, false, kBoardWidestPeriodTextWidth).statusWidth ==
                  kBoardWidestPeriodTextWidth + kBoardStatusSlack,
              "HALFTIME gets the 96px column it asks for on a 240px two-line board");
static_assert(boardStackedColumnsForBoard(240, false, kBoardWidestPeriodTextWidth).nameWidth == 70,
              "and the name that buys it is 70, not the floor");
static_assert(boardColumnsForBoard(240, false, kBoardWidestPeriodTextWidth).statusWidth <
                  kBoardWidestPeriodTextWidth,
              "where the one-line row on the same panel still cuts it");

// A board where no row reported a score releases the one score column, and a
// board with nothing in its status column spends the lot on the name.
static_assert(boardStackedColumnsForBoard(240, true, kBoardStartTimeTextWidth).scoreWidth == 0,
              "a two-line board of scheduled games draws no score column");
static_assert(boardStackedColumnsForBoard(240, true, kBoardStartTimeTextWidth).nameWidth == 106,
              "and gives its name 106px at the widest start time");
static_assert(boardStackedColumnsForBoard(240, false, kBoardStartTimeTextWidth).scoreWidth ==
                  kBoardStackedScoreWidth,
              "one game under way gives every two-line row a score column");
static_assert(boardStackedColumnsForBoard(240, false, 0).statusWidth == 0,
              "a board whose rows sent no status draws no status column");
static_assert(boardStackedColumnsForBoard(240, false, 0).nameWidth == 166,
              "and spends the lot on the name");

// The two lines sit at one x and differ in y, which is what lets one set of
// draw calls serve both orientations.
static_assert(boardStackedColumnsFor(240, false, kBoardStatusWidth).awayNameLeft ==
                  boardStackedColumnsFor(240, false, kBoardStatusWidth).homeNameLeft,
              "both lines of a two-line row start their name at one edge");
static_assert(boardStackedColumnsFor(240, false, kBoardStatusWidth).awayScoreRight ==
                  boardStackedColumnsFor(240, false, kBoardStatusWidth).homeScoreRight,
              "and end their score at one edge");

// The brace stands inside the content band with the name clear of it, and the
// row still ends on the glass.
static_assert(boardStackedBraceLeft() >= kCardMargin,
              "the brace stands inside the content band, not in the margin the chevrons use");
static_assert(boardStackedColumnsFor(240, false, kBoardStatusWidth).awayNameLeft ==
                  boardStackedBraceLeft() + kBoardBraceWidth + kBoardColumnGap,
              "and a column gap stands between the brace and the name");
static_assert(boardStackedColumnsFor(240, false, kBoardStatusWidth).statusRight <=
                  240 - kCardMargin,
              "240 two-line is on the glass");
static_assert(boardStackedColumnsFor(320, false, kBoardStatusWidth).statusRight <=
                  320 - kCardMargin,
              "320 two-line is on the glass");

// The separation is exactly two gaps and carries no parity term, because one
// name takes the whole remainder and nothing is split in half.
static_assert(boardStackedSeparationFor(240, false, kBoardStatusWidth) == 2 * kBoardColumnGap,
              "240 two-line with scores keeps two gaps of air");
static_assert(boardStackedSeparationFor(240, true, kBoardStartTimeWidth) == 2 * kBoardColumnGap,
              "240 two-line scoreless keeps two gaps of air");
static_assert(boardStackedSeparationFor(241, false, kBoardStatusWidth) == 2 * kBoardColumnGap,
              "and an odd panel width keeps two gaps as well, where the one-line row keeps 13");

// At the ceiling the one name column stands exactly on the floor it was
// derived from, which is the tightest of these: a pixel either way fails.
static_assert(boardStackedColumnsFor(240, false, boardStackedMaxStatusWidth(240, false))
                      .nameWidth == kBoardNameFloor,
              "240 two-line with scores holds the name floor at its ceiling");
static_assert(boardStackedColumnsFor(240, true, boardStackedMaxStatusWidth(240, true))
                      .nameWidth == kBoardNameFloor,
              "240 two-line scoreless holds the name floor at its ceiling");

}  // namespace CardColumns
