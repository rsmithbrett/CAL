// App/CardColumns.h: the scoreboard row's column arithmetic, over a range of
// panel widths rather than at the two the static_asserts read.
//
// Those assertions run in every arduino-cli build of both boards and are the
// right home for a property at a shipped width. What they cannot do is walk a
// range, and three of the things worth knowing about this function only show up
// when you do:
//
//   - The separation between the home block and the status column is exactly
//     2 * kBoardColumnGap on an even panel and one pixel more on an odd one,
//     for every width where the kBoardMinNameWidth floor is clear. The identity
//     TEXT_LAYOUT_DESIGN.md section 15c derives by substitution is the even
//     half of that; the odd pixel is the one the even split cannot divide, and
//     both shipped panels are even, so four assertions at those two widths
//     could not have said so.
//   - Where the floor engages, and what the row does below it. The doc names
//     174 and 202 as the widths the separation stops holding at; the first
//     width whose name column is clear of the floor is 176 and 204, because the
//     even split truncates.
//   - Whether a column keeps the slack it is written to carry. The header
//     asserts the start-time column holds the 78px the widest start time
//     measures; this asserts it holds that plus the 2px of slack, which is the
//     difference between a column sized against a measurement and one trimmed
//     down to it.
//
// TEST_PLAN.md section 18d has the mutations each case was proven against.

#include <cassert>
#include <iostream>

#include "CardColumns.h"

using CardColumns::boardColumns;
using CardColumns::boardColumnsFor;
using CardColumns::boardColumnsForBoard;
using CardColumns::BoardColumns;
using CardColumns::boardDefaultStatusWidth;
using CardColumns::boardLeftoverPixel;
using CardColumns::boardMaxStatusWidth;
using CardColumns::boardScoreWidth;
using CardColumns::boardSeparation;
using CardColumns::boardSeparationFor;
using CardColumns::boardStackedBraceLeft;
using CardColumns::boardStackedColumnsFor;
using CardColumns::boardStackedColumnsForBoard;
using CardColumns::boardStackedMaxStatusWidth;
using CardColumns::boardStackedScoreWidth;
using CardColumns::boardStackedSeparationFor;
using CardColumns::boardStackedStatusWidthFor;
using CardColumns::boardStatusLeft;
using CardColumns::boardStatusLeftFor;
using CardColumns::boardStatusWidthFor;
using CardColumns::kBoardBraceWidth;
using CardColumns::kBoardColumnGap;
using CardColumns::kBoardStackedScoreWidth;
using CardColumns::kBoardMinNameWidth;
using CardColumns::kBoardNameFloor;
using CardColumns::kBoardPeriodTextWidth;
using CardColumns::kBoardRowInset;
using CardColumns::kBoardScoreWidth;
using CardColumns::kBoardShortStartTimeTextWidth;
using CardColumns::kBoardStartTimeSlack;
using CardColumns::kBoardStartTimeTextWidth;
using CardColumns::kBoardStateWordWidth;
using CardColumns::kBoardStatusSlack;
using CardColumns::kBoardStatusWidth;
using CardColumns::kBoardWidestPeriodTextWidth;
using CardColumns::kCardMargin;

namespace {

/// The widest panel the sweep below covers. Far past anything the fleet has, so
/// a third board arriving at some width between the two it does have, or above
/// both, is already measured.
constexpr int kWidestSweep = 1024;

/// Whether the even split of the leftover pixels came out above the floor. Every
/// property that depends on an edge derived from the name width holds only here:
/// the floor clamps the width and nothing clamps the edges.
bool floorClear(int screenW, bool scoreless) {
  return boardColumns(screenW, scoreless).nameWidth > kBoardMinNameWidth;
}

/// The first width, counting up, whose name column is clear of the floor.
int firstWidthClearOfFloor(bool scoreless) {
  for (int w = 2 * (kCardMargin + kBoardRowInset); w <= kWidestSweep; ++w) {
    if (floorClear(w, scoreless)) return w;
  }
  return 0;
}

/// The two-line row at the status width its state asks for, which is the
/// counterpart of boardColumns() and the shape the boundary helpers below read.
/// The header has no two-argument form of it, for the reason boardColumns()
/// gives beside itself: an int converts to bool without a diagnostic, so a
/// two-arity overload would let a measurement be passed as a state.
BoardColumns stackedColumns(int screenW, bool scoreless) {
  return boardStackedColumnsFor(screenW, scoreless, boardDefaultStatusWidth(scoreless));
}

bool stackedFloorClear(int screenW, bool scoreless) {
  return stackedColumns(screenW, scoreless).nameWidth > kBoardMinNameWidth;
}

int firstStackedWidthClearOfFloor(bool scoreless) {
  for (int w = 2 * (kCardMargin + kBoardRowInset); w <= kWidestSweep; ++w) {
    if (stackedFloorClear(w, scoreless)) return w;
  }
  return 0;
}

int stackedSeparation(int screenW, bool scoreless) {
  return boardStackedSeparationFor(screenW, scoreless, boardDefaultStatusWidth(scoreless));
}

/// A team name and what it measures in FreeSansBold9pt7b, off the vendored
/// glyph table by the method TEXT_LAYOUT_DESIGN.md section 9b describes. The
/// same reimplementation reproduces all twenty figures this repository already
/// carries - FINAL 55, HALFTIME 94, Hurricanes 94, Capitals 70 and the rest -
/// to the pixel, which is what these are worth trusting on.
///
/// Five of these are the names the photographed board cut to an ellipsis.
struct Name {
  const char* team;
  int measured;
};

constexpr Name kPhotographedNames[] = {
    {"Jets", 36},    {"Hurricanes", 94}, {"Wild", 38},   {"Capitals", 70},
    {"Mammoth", 86}, {"Flyers", 53},     {"Oilers", 51}, {"Rangers", 72},
};

/// Names above the column either form draws. The point of the two-line row is
/// how much of the league moves across that line, not that the line goes away.
constexpr Name kNamesStillTooWide[] = {
    {"Golden Knights", 134},
    {"Diamondbacks", 128},
};

}  // namespace

int main() {
  // --- what a column has to hold --------------------------------------------
  //
  // First, because this is the one thing in this file that reads a measurement
  // of a string rather than an arithmetic property of a width.
  //
  // The widest start time is 78px of FreeSansBold9pt7b and the column carries
  // 2px of slack on top. Reading both names apart is what separates a column
  // trimmed towards the measurement from one that still has its slack: a status
  // width of 79 clears the header's assertion and fails here.
  for (const int width : {320, 240}) {
    assert(boardColumns(width, true).statusWidth >=
           kBoardStartTimeTextWidth + kBoardStartTimeSlack);
    // A row with scores draws a state word there. FINAL is 55px and the column
    // is 56. It is not sized for a period, and the periods a provider sends do
    // not fit it - see the header and TEXT_LAYOUT_DESIGN.md section 15g.
    assert(boardColumns(width, false).statusWidth == kBoardStatusWidth);
    assert(boardColumns(width, false).statusWidth >= kBoardStateWordWidth);
    // And two digits of score.
    assert(boardColumns(width, false).scoreWidth == kBoardScoreWidth);
    assert(boardColumns(width, true).scoreWidth == 0);
  }

  // --- the table in TEXT_LAYOUT_DESIGN.md section 15b ------------------------
  //
  // The four rows of the "this change" column, as equalities rather than as
  // floors. The assertions in the header hold the name columns at or above the
  // figures v2026.10.31.0002 shipped, which is what stops them shrinking; these
  // are what makes the table in the design document a thing a build checks
  // rather than a thing somebody re-derives.
  assert(boardColumns(320, true).nameWidth == 97);
  assert(boardColumns(320, true).statusWidth == 80);
  assert(boardColumns(320, false).nameWidth == 83);
  assert(boardColumns(320, false).statusWidth == 56);
  assert(boardColumns(240, true).nameWidth == 57);
  assert(boardColumns(240, true).statusWidth == 80);
  assert(boardColumns(240, false).nameWidth == 43);
  assert(boardColumns(240, false).statusWidth == 56);

  // --- the column a measured status gets ------------------------------------
  //
  // TEXT_LAYOUT_DESIGN.md section 15h. The layout used to pick the status width
  // from the row's score strings and then draw the status string into it; it
  // picks the width from the string now, and these are the two rows that were
  // cut before it did.
  //
  // The period case is an equality, because a `>=` would be satisfied at both
  // scoreless widths by the 80px the shipped layout already handed those rows,
  // and would read as a property it was not testing. The start-time case stays
  // a `>=` because 80 is what it is asking for.
  for (const int width : {320, 240}) {
    for (const bool scoreless : {true, false}) {
      // 15g: a live game's period, on a row that has scores and on one that
      // does not. "6TH INN" is 74px and the column it gets is 76 at all four.
      assert(boardStatusWidthFor(width, scoreless, kBoardPeriodTextWidth) ==
             kBoardPeriodTextWidth + kBoardStatusSlack);
      // 15f: a scheduled game that reported one side's score is not scoreless,
      // and its status is still a start time. The widest is 78px.
      assert(boardStatusWidthFor(width, scoreless, kBoardStartTimeTextWidth) >=
             kBoardStartTimeTextWidth);
    }
  }

  // The state's own default widths, as literals. Written as
  // kBoardStartTimeTextWidth + kBoardStartTimeSlack this cannot fail, because
  // kBoardStartTimeWidth is defined as that sum and boardDefaultStatusWidth()
  // returns it: the two sides move together under every mutation of either
  // constant.
  assert(boardDefaultStatusWidth(true) == 80);
  assert(boardDefaultStatusWidth(false) == 56);
  assert(boardScoreWidth(true) == 0);
  assert(boardScoreWidth(false) == 26);

  // The three rules in 15h, read at the boundaries between them rather than in
  // the middle of each. 55 asks for 56, 56 asks for 56, 57 asks for 59, and an
  // empty status asks for nothing because nothing is drawn there.
  //
  // Literals on the right, for the reason above: a right-hand side built from
  // the same constants as the left moves with them.
  for (const int width : {320, 240}) {
    for (const bool scoreless : {true, false}) {
      assert(boardStatusWidthFor(width, scoreless, 0) == 0);
      assert(boardStatusWidthFor(width, scoreless, kBoardStateWordWidth) == 56);
      assert(boardStatusWidthFor(width, scoreless, kBoardStatusWidth) == 56);
      assert(boardStatusWidthFor(width, scoreless, kBoardStatusWidth + 1) == 59);
      // The slack is spent by the ceiling rather than carried on top of it. As
      // `<= kBoardStartTimeTextWidth + kBoardStatusSlack` this was a
      // restatement of boardStatusWidthFor()'s own min(); at 80 and 80 it reads
      // the two shipped widths, both of which have the room.
      assert(boardStatusWidthFor(width, scoreless, kBoardStartTimeTextWidth) == 80);
    }
  }

  // An empty status hands the names the whole row.
  assert(boardColumnsFor(320, false, 0).nameWidth == 111);
  assert(boardColumnsFor(240, false, 0).nameWidth == 71);
  assert(boardColumnsFor(320, true, 0).nameWidth == 137);
  assert(boardColumnsFor(240, true, 0).nameWidth == 97);

  // Section 15h's table, as equalities: the status width a measured string gets
  // and the name column left beside it, at both panel widths in both states.
  struct MeasuredRow {
    int width;
    bool scoreless;
    int measured;
    int statusWidth;
    int nameWidth;
  };
  const MeasuredRow kMeasured[] = {
      {320, false, 37, 56, 83},   // PPD
      {320, false, 55, 56, 83},   // FINAL
      {320, false, 67, 69, 76},   // 7:30 PM, on a row that reported one score
      {320, false, 74, 76, 73},   // 6TH INN
      {320, false, 78, 80, 71},   // 12:00 AM
      {320, false, 80, 82, 70},   // 3RD QTR
      {320, false, 94, 96, 63},   // HALFTIME
      {320, true, 37, 56, 109},   // PPD, which used to take the 80px column
      {320, true, 78, 80, 97},    // 12:00 AM
      {320, true, 80, 82, 96},    // 3RD QTR, a period on a row with no score sent
      {320, true, 94, 96, 89},    // HALFTIME
      {240, false, 37, 56, 43},   // PPD
      {240, false, 55, 56, 43},   // FINAL
      {240, false, 67, 69, 36},   // 7:30 PM
      {240, false, 74, 76, 33},   // 6TH INN
      {240, false, 78, 80, 31},   // 12:00 AM, on the name floor
      {240, false, 80, 80, 31},   // 3RD QTR, at the ceiling with no slack left
      {240, false, 94, 80, 31},   // HALFTIME, cut - see below
      {240, true, 37, 56, 69},    // PPD
      {240, true, 78, 80, 57},    // 12:00 AM
      {240, true, 80, 82, 56},    // 3RD QTR
      {240, true, 94, 96, 49},    // HALFTIME
  };
  for (const MeasuredRow& row : kMeasured) {
    const int statusWidth = boardStatusWidthFor(row.width, row.scoreless, row.measured);
    assert(statusWidth == row.statusWidth);
    assert(boardColumnsFor(row.width, row.scoreless, statusWidth).nameWidth == row.nameWidth);
  }

  // The one row the column still does not hold: "HALFTIME" at 94px on a 240px
  // panel that has scores. The ceiling there is 80, because 96 would put the
  // names at 23 and kBoardMinNameWidth is 24. Asserted as the gap it is, so a
  // change that closes it has to come through this line and section 15h.
  assert(boardMaxStatusWidth(240, false) == 80);
  assert(boardStatusWidthFor(240, false, kBoardWidestPeriodTextWidth) <
         kBoardWidestPeriodTextWidth);
  // The other three combinations do hold it.
  assert(boardStatusWidthFor(320, false, kBoardWidestPeriodTextWidth) >=
         kBoardWidestPeriodTextWidth);
  assert(boardStatusWidthFor(320, true, kBoardWidestPeriodTextWidth) >=
         kBoardWidestPeriodTextWidth);
  assert(boardStatusWidthFor(240, true, kBoardWidestPeriodTextWidth) >=
         kBoardWidestPeriodTextWidth);

  // The ceilings in 15h's own table.
  assert(boardMaxStatusWidth(320, true) == 212);
  assert(boardMaxStatusWidth(320, false) == 160);
  assert(boardMaxStatusWidth(240, true) == 132);

  // --- what a row asks for on its own ---------------------------------------
  //
  // boardColumnsFor() takes one row's status, so what it returns is what that
  // row would take if it were the only one on the card. Four rows with scores
  // on a 320px panel, by what their status measures, and the four grids they
  // ask for. Until section 15i they were four grids one board drew at once;
  // they are what the board picks the widest of now, and the figures are the
  // same either way.
  {
    const int statuses[] = {0, kBoardStateWordWidth, kBoardPeriodTextWidth,
                            kBoardWidestPeriodTextWidth};
    const int homeNameLefts[] = {157, 129, 119, 109};
    const int awayScoreRights[] = {151, 123, 113, 103};
    const int homeScoreRights[] = {294, 238, 218, 198};
    for (int i = 0; i < 4; ++i) {
      const BoardColumns c =
          boardColumnsFor(320, false, boardStatusWidthFor(320, false, statuses[i]));
      assert(c.homeNameLeft == homeNameLefts[i]);
      assert(c.awayScoreRight == awayScoreRights[i]);
      assert(c.homeScoreRight == homeScoreRights[i]);
      // Whatever a row's status does to the other four edges, the status column's
      // own right edge is the same on every row, which is the edge it is aligned
      // to.
      assert(c.statusRight == 320 - kCardMargin - kBoardRowInset);
    }
    // 48px between the widest and narrowest home name edge on one board, and
    // 96px between the home score edges. Read back out of the function rather
    // than subtracted from the two literal arrays above, which would have been
    // arithmetic on this file's own figures and read nothing from the header.
    const BoardColumns widest = boardColumnsFor(320, false, boardStatusWidthFor(320, false, 0));
    const BoardColumns narrowest = boardColumnsFor(
        320, false, boardStatusWidthFor(320, false, kBoardWidestPeriodTextWidth));
    assert(widest.homeNameLeft - narrowest.homeNameLeft == 48);
    assert(widest.homeScoreRight - narrowest.homeScoreRight == 96);
  }

  // --- four rows of one board are one table ---------------------------------
  //
  // TEXT_LAYOUT_DESIGN.md section 15i. boardColumnsForBoard() takes the board's
  // own scoreless and the widest status any of its rows measures, and every row
  // draws into what it returns. The 48px and 96px drift the block above records
  // is what a board used to carry; here it is zero, read the same way.
  {
    // The four statuses of the 320px board above, laid on one grid. Every row
    // lands where the widest of them asked to land, which is the HALFTIME row's
    // 109, 103 and 198.
    const int statuses[] = {0, kBoardStateWordWidth, kBoardPeriodTextWidth,
                            kBoardWidestPeriodTextWidth};
    int widestStatus = 0;
    for (const int measured : statuses) {
      if (measured > widestStatus) widestStatus = measured;
    }
    const BoardColumns grid = boardColumnsForBoard(320, false, widestStatus);
    assert(grid.homeNameLeft == 109);
    assert(grid.awayScoreRight == 103);
    assert(grid.homeScoreRight == 198);
    assert(grid.nameWidth == 63);
    assert(grid.statusWidth == 96);

    // And the grid is the narrowest-named of the four a row could have asked
    // for, met by exactly the row that asked for it. Not a restatement of the
    // fold: it reads the board's answer against boardColumnsFor()'s, which is
    // the function the loop used to call, so a board that took the first row's
    // string or the last one's fails here.
    int metTheBoard = 0;
    for (const int measured : statuses) {
      const BoardColumns own =
          boardColumnsFor(320, false, boardStatusWidthFor(320, false, measured));
      assert(own.nameWidth >= grid.nameWidth);
      assert(own.statusWidth <= grid.statusWidth);
      if (own.nameWidth == grid.nameWidth) ++metTheBoard;
    }
    assert(metTheBoard == 1);

    // The photographed board, both panels. FINAL at 55, P1 at 22 twice and a
    // 7:00 PM start time at 67, measured off the vendored FreeSansBold9pt7b
    // table by section 15a's method, which reproduces every 9pt figure in that
    // document to the pixel. One row has scores, so the board is not scoreless
    // and all four rows draw a score column.
    const int photographed[] = {kBoardStateWordWidth, 22, 22, 67};
    int widestPhotographed = 0;
    for (const int measured : photographed) {
      if (measured > widestPhotographed) widestPhotographed = measured;
    }
    assert(widestPhotographed == 67);

    const BoardColumns narrow = boardColumnsForBoard(240, false, widestPhotographed);
    assert(narrow.statusWidth == 69);
    assert(narrow.nameWidth == 36);
    assert(narrow.homeNameLeft == 82);
    assert(narrow.homeScoreRight == 144);
    assert(narrow.scoreWidth == kBoardScoreWidth);

    const BoardColumns wide = boardColumnsForBoard(320, false, widestPhotographed);
    assert(wide.statusWidth == 69);
    assert(wide.nameWidth == 76);

    // A board of state words costs its names nothing at either width, which is
    // the other half of the trade.
    assert(boardColumnsForBoard(320, false, kBoardStateWordWidth).nameWidth == 83);
    assert(boardColumnsForBoard(240, false, kBoardStateWordWidth).nameWidth == 43);

    // A board where no row reported a score releases both score columns on
    // every row of it, and one that did gives every row one. The board's
    // scoreless, not a row's.
    assert(boardColumnsForBoard(240, true, kBoardStartTimeTextWidth).scoreWidth == 0);
    assert(boardColumnsForBoard(240, true, kBoardStartTimeTextWidth).nameWidth == 57);
    assert(boardColumnsForBoard(240, false, kBoardStartTimeTextWidth).scoreWidth ==
           kBoardScoreWidth);
    assert(boardColumnsForBoard(240, false, kBoardStartTimeTextWidth).nameWidth ==
           kBoardNameFloor);
  }

  // --- the fold runs over the strings, not over the columns ------------------
  //
  // boardColumnsForBoard() takes the widest measurement rather than the widest
  // column, which is only the same thing while boardStatusWidthFor() does not
  // fall as its measurement rises. The draw path folds four ints and converts
  // once on the strength of that, so it is asserted rather than reasoned about
  // in a remark.
  //
  // Both halves, at every swept width and in both states: the function is
  // monotonic, and the column the widest string asks for is the widest column
  // any of the strings asks for.
  for (int width = 2 * (kCardMargin + kBoardRowInset); width <= kWidestSweep; ++width) {
    for (const bool scoreless : {true, false}) {
      int previous = boardStatusWidthFor(width, scoreless, 0);
      for (int measured = 1; measured <= 320; ++measured) {
        const int current = boardStatusWidthFor(width, scoreless, measured);
        assert(current >= previous);
        previous = current;
      }

      // The eight measured widths the sweep below reads, as a board. 300 is
      // past the ceiling at every width, so the last two agree there and
      // disagree below it, which is the case a `>=` would have missed.
      const int measured[] = {0, 55, 56, 57, 74, 78, 94, 300};
      int widestMeasured = 0;
      int widestColumn = 0;
      for (const int m : measured) {
        if (m > widestMeasured) widestMeasured = m;
        const int column = boardStatusWidthFor(width, scoreless, m);
        if (column > widestColumn) widestColumn = column;
      }
      assert(boardStatusWidthFor(width, scoreless, widestMeasured) == widestColumn);
      assert(boardColumnsForBoard(width, scoreless, widestMeasured).statusWidth == widestColumn);
    }
  }

  // --- the sweep ------------------------------------------------------------
  for (int width = 2 * (kCardMargin + kBoardRowInset); width <= kWidestSweep; ++width) {
    for (const bool scoreless : {true, false}) {
      const BoardColumns c = boardColumns(width, scoreless);

      // Both ends of the row sit the same distance in from the glass.
      assert(c.awayNameLeft == kCardMargin + kBoardRowInset);
      assert(c.statusRight == width - kCardMargin - kBoardRowInset);

      // Each block is its name column plus its score column, and the two blocks
      // are the same size.
      assert(c.awayScoreRight - c.awayNameLeft == c.nameWidth + c.scoreWidth);
      assert(c.homeScoreRight - c.homeNameLeft == c.nameWidth + c.scoreWidth);

      // A gap between the away block and the home block, so two names cannot be
      // drawn against each other.
      assert(c.homeNameLeft - c.awayScoreRight == kBoardColumnGap);

      // The blocks run left to right.
      assert(c.awayNameLeft < c.awayScoreRight);
      assert(c.awayScoreRight < c.homeNameLeft);
      assert(c.homeNameLeft < c.homeScoreRight);

      // The name column never goes under the floor, whether the even split put
      // it there or not.
      assert(c.nameWidth >= kBoardMinNameWidth);

      if (!floorClear(width, scoreless)) continue;

      // The row never asks for more pixels than it has, and what it leaves over
      // is the one pixel the even split cannot divide. Both fixed widths are
      // even, so that pixel is there on an odd panel and not on an even one.
      const int asked = 2 * c.nameWidth + 2 * c.scoreWidth + c.statusWidth + 3 * kBoardColumnGap;
      const int band = c.statusRight - c.awayNameLeft;
      assert(asked <= band);
      assert(band - asked == width % 2);

      // The separation is an identity rather than a bound: substituting
      // `shared` back gives 2 * kBoardColumnGap plus that leftover pixel, for
      // any status width, score width and panel width. The header asserts it is
      // at least 2 * kBoardColumnGap at four points, all of them even; this is
      // what says where the thirteenth pixel comes from.
      assert(boardSeparation(width, scoreless) == 2 * kBoardColumnGap + width % 2);
      assert(c.homeScoreRight ==
             c.statusRight - c.statusWidth - 2 * kBoardColumnGap - width % 2);

      // The status column starts after the home block ends and finishes on the
      // glass.
      assert(boardStatusLeft(width, scoreless) > c.homeScoreRight);
      assert(c.statusRight <= width - kCardMargin);
    }
  }

  // --- where the floor engages ----------------------------------------------
  //
  // Below these widths the floor clamps the name column and the edges derived
  // from it carry on shrinking, so the home block walks towards the status
  // column. 174 and 202 are where the separation stops being
  // 2 * kBoardColumnGap. The first width whose name column is clear of the floor
  // is two wider in each case, because the leftover pixels are split with
  // integer division and 49 pixels of leftover still give a 24px name.
  assert(firstWidthClearOfFloor(true) == 176);
  assert(firstWidthClearOfFloor(false) == 204);
  assert(boardColumns(175, true).nameWidth == kBoardMinNameWidth);
  assert(boardColumns(203, false).nameWidth == kBoardMinNameWidth);

  // The separation holds down to the widths the document names and not below.
  assert(boardSeparation(174, true) == 2 * kBoardColumnGap);
  assert(boardSeparation(173, true) < 2 * kBoardColumnGap);
  assert(boardSeparation(202, false) == 2 * kBoardColumnGap);
  assert(boardSeparation(201, false) < 2 * kBoardColumnGap);

  // And the columns themselves meet lower still: 162 and 190 are where the home
  // block's right edge reaches the status column's left edge, and under those
  // the two are drawn through each other. Both figures are well under every
  // panel the fleet has, which is what makes this latent rather than live - and
  // what a third board arriving narrow would land on.
  assert(boardSeparation(162, true) == 0);
  assert(boardSeparation(161, true) < 0);
  assert(boardSeparation(190, false) == 0);
  assert(boardSeparation(189, false) < 0);

  // Both shipped panels are clear of all four figures.
  for (const int width : {320, 240}) {
    for (const bool scoreless : {true, false}) {
      assert(floorClear(width, scoreless));
      assert(boardSeparation(width, scoreless) == 2 * kBoardColumnGap);
    }
  }

  // --- the sweep again, with the status width coming from a string -----------
  //
  // The same range and the same properties, over eight measured widths rather
  // than the state's own two. One of the eight, 57, gives an odd status width
  // of 59, and 300 gives the ceiling, which is odd at every odd panel width the
  // names set it at - above 410 scoreless and 462 with scores it is a flat 302,
  // and below 164 and 216 the even 56.
  // That is what the separation identity needed: every status width in the
  // header was even until one was measured off a glyph table, so the leftover
  // pixel read as the panel width's own parity and section 15c said it was.
  // boardLeftoverPixel() is that parity read off the row's leftover instead.
  for (int width = 2 * (kCardMargin + kBoardRowInset); width <= kWidestSweep; ++width) {
    for (const bool scoreless : {true, false}) {
      const int ceiling = boardMaxStatusWidth(width, scoreless);

      // No panel width can produce a column narrower than the one the fleet
      // draws, which is what stops the ceiling's arithmetic going negative on a
      // narrow board and taking the status column with it.
      assert(ceiling >= kBoardStatusWidth);

      // A column at the ceiling leaves the name floor standing exactly, which
      // is what the ceiling is derived from. Only where the names set it: below
      // that the ceiling is the kBoardStatusWidth clamp and the narrower
      // kBoardMinNameWidth floor is the one in play.
      if (ceiling > kBoardStatusWidth) {
        assert(boardColumnsFor(width, scoreless, ceiling).nameWidth == kBoardNameFloor);
      }

      for (const int measured : {0, 55, 56, 57, 74, 78, 94, 300}) {
        const int statusWidth = boardStatusWidthFor(width, scoreless, measured);
        assert(statusWidth == 0 || statusWidth >= kBoardStatusWidth);
        assert(statusWidth <= ceiling);

        const BoardColumns c = boardColumnsFor(width, scoreless, statusWidth);
        assert(c.statusWidth == statusWidth);
        assert(c.nameWidth >= kBoardMinNameWidth);
        assert(c.awayNameLeft == kCardMargin + kBoardRowInset);
        assert(c.statusRight == width - kCardMargin - kBoardRowInset);
        assert(c.homeNameLeft - c.awayScoreRight == kBoardColumnGap);

        if (c.nameWidth <= kBoardMinNameWidth) continue;

        const int asked =
            2 * c.nameWidth + 2 * c.scoreWidth + c.statusWidth + 3 * kBoardColumnGap;
        const int band = c.statusRight - c.awayNameLeft;
        assert(asked <= band);
        assert(band - asked == boardLeftoverPixel(width, scoreless, statusWidth));
        assert(boardSeparationFor(width, scoreless, statusWidth) ==
               2 * kBoardColumnGap + boardLeftoverPixel(width, scoreless, statusWidth));
        assert(boardStatusLeftFor(width, scoreless, statusWidth) > c.homeScoreRight);
        assert(c.statusRight <= width - kCardMargin);
      }
    }
  }

  // --- the two-line row, TEXT_LAYOUT_DESIGN.md section 15j -------------------
  //
  // A profile panel has height going spare and width it has run out of, so the
  // row turns one into the other: two lines, one team per line, joined by a
  // drawn brace. One name instead of two is the whole of the width win, and
  // everything below is what that costs and what it buys.

  // The photographed board. Four NHL games reading FINAL, FINAL, P3 and a
  // 6:00 PM start time, so the widest status on it is the start time's 67px
  // and not FINAL's 55, and the column the board lands on is 69.
  {
    const BoardColumns twoLine =
        boardStackedColumnsForBoard(240, false, kBoardShortStartTimeTextWidth);
    const BoardColumns oneLine =
        boardColumnsForBoard(240, false, kBoardShortStartTimeTextWidth);
    assert(oneLine.nameWidth == 36);
    assert(twoLine.nameWidth == 97);
    assert(twoLine.statusWidth == kBoardShortStartTimeTextWidth + kBoardStatusSlack);
    assert(oneLine.statusWidth == twoLine.statusWidth);
    assert(twoLine.scoreWidth == kBoardStackedScoreWidth);

    // The edges, as section 15j writes them down.
    assert(boardStackedBraceLeft() == 14);
    assert(twoLine.awayNameLeft == 28);
    assert(twoLine.awayNameLeft + twoLine.nameWidth - 1 == 124);
    assert(twoLine.awayScoreRight == 145);
    assert(twoLine.statusRight - twoLine.statusWidth == 157);
    assert(twoLine.statusRight == 226);
    assert(boardStackedSeparationFor(240, false, twoLine.statusWidth) == 12);

    // And the claim the change is for: all eight of those names are drawn
    // whole at 97 and seven of the eight are not at 36. Counting both sides
    // rather than asserting the eight fit, so a name added to the table that
    // the one-line row already held cannot read as a win.
    int cutOnMain = 0;
    for (const Name& name : kPhotographedNames) {
      assert(name.measured <= twoLine.nameWidth);
      if (name.measured > oneLine.nameWidth) ++cutOnMain;
    }
    assert(cutOnMain == 7);

    // What it does not reach. The 20-character kMaxName holds strings no panel
    // width makes fit, and these two are still ellipsized.
    for (const Name& name : kNamesStillTooWide) {
      assert(name.measured > twoLine.nameWidth);
    }
  }

  // A board of state words, where the status fits the 56px column and costs
  // the name nothing in either form.
  assert(boardColumnsForBoard(240, false, kBoardStateWordWidth).nameWidth == 43);
  assert(boardStackedColumnsForBoard(240, false, kBoardStateWordWidth).nameWidth == 110);
  assert(boardStackedColumnsForBoard(240, false, kBoardStateWordWidth).statusWidth ==
         kBoardStatusWidth);

  // HALFTIME, which 15h and 15i both record as the one string the profile
  // board could not hold. One kBoardNameFloor in the ceiling instead of two is
  // the whole of the fix, so the ceilings are what this reads first.
  assert(boardStackedMaxStatusWidth(240, false) == 135);
  assert(boardStackedMaxStatusWidth(240, true) == 155);
  assert(boardMaxStatusWidth(240, false) == 80);
  {
    const int wanted = kBoardWidestPeriodTextWidth + kBoardStatusSlack;
    assert(boardStackedStatusWidthFor(240, false, kBoardWidestPeriodTextWidth) == wanted);
    assert(boardStatusWidthFor(240, false, kBoardWidestPeriodTextWidth) < wanted);
    assert(boardStackedColumnsForBoard(240, false, kBoardWidestPeriodTextWidth).nameWidth == 70);
    // OVERTIME at 97 fits too, where the one-line row cuts it by 19 pixels.
    assert(boardStackedStatusWidthFor(240, false, 97) == 99);
  }

  // The three rules boardStackedStatusWidthFor() states, pinned at the
  // boundary between each pair of them. Nothing in App/CardColumns.h's own
  // assertions reads 56 or 57, so a rule that fires at `< kBoardStatusWidth`
  // rather than `<=` passes every one of them and only shows up here: it hands
  // a 56px string a 58px column, which is within every bound the sweep below
  // checks and is not the rule.
  {
    struct Rule {
      int measured;
      int want;
    };
    const Rule kRules[] = {
        {0, 0},   {1, kBoardStatusWidth},      {55, kBoardStatusWidth},
        {56, kBoardStatusWidth},               {57, 57 + kBoardStatusSlack},
        {74, 74 + kBoardStatusSlack},          {94, 94 + kBoardStatusSlack},
    };
    for (const Rule& rule : kRules) {
      assert(boardStackedStatusWidthFor(240, false, rule.measured) == rule.want);
      assert(boardStackedStatusWidthFor(320, false, rule.measured) == rule.want);
    }
    // An empty status also "fits" 56, and the rule that owns zero reads first.
    assert(boardStackedStatusWidthFor(240, false, 0) == 0);
    assert(boardStackedStatusWidthFor(240, false, -1) == 0);
  }

  // The rest of section 15j's table, as equalities. The 320 rows are what a
  // 320px panel in profile would draw; no board on the fleet is one, and the
  // sweep below is what covers the widths between and beyond.
  struct Row {
    int panel;
    bool scoreless;
    int measured;
    int nameBefore;
    int nameAfter;
    int statusAfter;
  };
  const Row kSection15j[] = {
      {240, false, 0, 71, 166, 0},    {240, false, 37, 43, 110, 56},
      {240, false, 55, 43, 110, 56},  {240, false, 67, 36, 97, 69},
      {240, false, 74, 33, 90, 76},   {240, false, 78, 31, 86, 80},
      {240, false, 80, 31, 84, 82},   {240, false, 94, 31, 70, 96},
      {240, false, 97, 31, 67, 99},   {240, true, 78, 57, 106, 80},
      {240, true, 94, 49, 90, 96},    {320, false, 55, 83, 190, 56},
      {320, false, 94, 63, 150, 96},  {320, true, 94, 89, 170, 96},
  };
  for (const Row& row : kSection15j) {
    const BoardColumns before = boardColumnsForBoard(row.panel, row.scoreless, row.measured);
    const BoardColumns after =
        boardStackedColumnsForBoard(row.panel, row.scoreless, row.measured);
    assert(before.nameWidth == row.nameBefore);
    assert(after.nameWidth == row.nameAfter);
    assert(after.statusWidth == row.statusAfter);
    // Every row of that table is a wider name than the one-line grid drew, at
    // every status a provider can send. That is the table's whole claim and
    // nothing above states it as a rule.
    assert(after.nameWidth > before.nameWidth);
  }

  // --- the two-line sweep ----------------------------------------------------
  //
  // The same range and the same eight measured widths the one-line sweep uses.
  // Two properties here are the one-line row's with the parity term taken out,
  // because one name takes the whole remainder and nothing is split in half.
  for (int width = 2 * (kCardMargin + kBoardRowInset); width <= kWidestSweep; ++width) {
    for (const bool scoreless : {true, false}) {
      const int ceiling = boardStackedMaxStatusWidth(width, scoreless);
      assert(ceiling >= kBoardStatusWidth);

      // A column at the ceiling leaves the one name column standing exactly on
      // the floor the ceiling is derived from, wherever the name sets it.
      if (ceiling > kBoardStatusWidth) {
        assert(boardStackedColumnsFor(width, scoreless, ceiling).nameWidth == kBoardNameFloor);
      }

      // The two-line ceiling is never under the one-line ceiling at the same
      // width and state: it subtracts one name floor where the other subtracts
      // two, and it releases a score column as well. This is what says the
      // strings the one-line row cut are reachable rather than only that
      // HALFTIME is.
      assert(ceiling >= boardMaxStatusWidth(width, scoreless));

      int previousStatus = -1;
      for (const int measured : {0, 55, 56, 57, 74, 78, 94, 300}) {
        const int statusWidth = boardStackedStatusWidthFor(width, scoreless, measured);
        assert(statusWidth == 0 || statusWidth >= kBoardStatusWidth);
        assert(statusWidth <= ceiling);

        // Never falls as the measurement rises. This is what lets
        // showSportsBoard() fold over the board's measurements and convert
        // once, rather than converting each row and keeping the widest column.
        assert(statusWidth >= previousStatus);
        previousStatus = statusWidth;

        const BoardColumns c = boardStackedColumnsFor(width, scoreless, statusWidth);
        assert(c.statusWidth == statusWidth);
        assert(c.scoreWidth == boardStackedScoreWidth(scoreless));
        assert(c.nameWidth >= kBoardMinNameWidth);

        // Both lines at one x, which is what keeps showSportsBoard() to one set
        // of draw calls for the two orientations.
        assert(c.awayNameLeft == c.homeNameLeft);
        assert(c.awayScoreRight == c.homeScoreRight);

        // The brace stands at the row's left edge with a column gap between it
        // and the name, and the row ends on the glass.
        assert(boardStackedBraceLeft() == kCardMargin + kBoardRowInset);
        assert(c.awayNameLeft == boardStackedBraceLeft() + kBoardBraceWidth + kBoardColumnGap);
        assert(c.statusRight == width - kCardMargin - kBoardRowInset);
        assert(c.statusRight <= width - kCardMargin);

        if (c.nameWidth <= kBoardMinNameWidth) continue;

        // What the row asks for is what it has, with nothing left over. The
        // one-line sweep's counterpart carries boardLeftoverPixel() here.
        const int asked =
            kBoardBraceWidth + c.nameWidth + c.scoreWidth + c.statusWidth + 3 * kBoardColumnGap;
        const int band = c.statusRight - boardStackedBraceLeft();
        assert(asked == band);

        // So the separation is two gaps exactly, at an odd panel width as well
        // as an even one.
        assert(boardStackedSeparationFor(width, scoreless, statusWidth) == 2 * kBoardColumnGap);
        assert(c.statusRight - c.statusWidth > c.awayScoreRight);
      }
    }
  }

  // Where the two-line floor engages, which is far below either shipped panel
  // and is what a third board arriving narrow would land on. The separation
  // degrades below it for the one-line row's reason: the floor clamps the name
  // width and nothing clamps the edges derived from it.
  assert(firstStackedWidthClearOfFloor(false) == 155);
  assert(firstStackedWidthClearOfFloor(true) == 159);
  assert(stackedSeparation(154, false) == 2 * kBoardColumnGap);
  assert(stackedSeparation(153, false) < 2 * kBoardColumnGap);
  assert(stackedSeparation(158, true) == 2 * kBoardColumnGap);
  assert(stackedSeparation(157, true) < 2 * kBoardColumnGap);
  assert(stackedSeparation(142, false) <= 0);
  assert(stackedSeparation(146, true) <= 0);

  // Both shipped panels are clear of all of it, in both states.
  for (const int width : {320, 240}) {
    for (const bool scoreless : {true, false}) {
      assert(stackedFloorClear(width, scoreless));
      assert(stackedSeparation(width, scoreless) == 2 * kBoardColumnGap);
    }
  }

  std::cout << "PASS: scoreboard columns - the design document's two tables, the measured "
               "status column and its ceiling, what a row asks for on its own, the one grid "
               "four rows of a board share, the fold over the strings at every width up to "
            << kWidestSweep
            << " in both states, the separation identity over eight status widths, the "
               "floor's boundaries, and section 15j's two-line row - the photographed board's "
               "eight names, the table at both widths, HALFTIME held whole, and the same sweep "
               "with the parity term gone\n";
}
