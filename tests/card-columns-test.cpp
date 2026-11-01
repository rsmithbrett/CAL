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
using CardColumns::BoardColumns;
using CardColumns::boardDefaultStatusWidth;
using CardColumns::boardLeftoverPixel;
using CardColumns::boardMaxStatusWidth;
using CardColumns::boardScoreWidth;
using CardColumns::boardSeparation;
using CardColumns::boardSeparationFor;
using CardColumns::boardStatusLeft;
using CardColumns::boardStatusLeftFor;
using CardColumns::boardStatusWidthFor;
using CardColumns::kBoardColumnGap;
using CardColumns::kBoardMinNameWidth;
using CardColumns::kBoardNameFloor;
using CardColumns::kBoardPeriodTextWidth;
using CardColumns::kBoardRowInset;
using CardColumns::kBoardScoreWidth;
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

  // The state's own default widths, which are what the two-argument form and
  // the header's assertions read.
  assert(boardDefaultStatusWidth(true) == kBoardStartTimeTextWidth + kBoardStartTimeSlack);
  assert(boardDefaultStatusWidth(false) == kBoardStatusWidth);
  assert(boardScoreWidth(true) == 0);
  assert(boardScoreWidth(false) == kBoardScoreWidth);

  // The three rules in 15h, read at the boundaries between them rather than in
  // the middle of each. 55 asks for 56, 56 asks for 56, 57 asks for 59, and an
  // empty status asks for nothing because nothing is drawn there.
  for (const int width : {320, 240}) {
    for (const bool scoreless : {true, false}) {
      assert(boardStatusWidthFor(width, scoreless, 0) == 0);
      assert(boardStatusWidthFor(width, scoreless, kBoardStateWordWidth) == kBoardStatusWidth);
      assert(boardStatusWidthFor(width, scoreless, kBoardStatusWidth) == kBoardStatusWidth);
      assert(boardStatusWidthFor(width, scoreless, kBoardStatusWidth + 1) ==
             kBoardStatusWidth + 1 + kBoardStatusSlack);
      // And the slack is the first thing a ceiling spends, not an allowance on
      // top of it.
      assert(boardStatusWidthFor(width, scoreless, kBoardStartTimeTextWidth) <=
             kBoardStartTimeTextWidth + kBoardStatusSlack);
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

  // --- what four rows of one board no longer share --------------------------
  //
  // boardColumnsFor() is called per row, so a row's status string now moves the
  // home name's left edge and both score columns. Four rows with scores on a
  // 320px panel, by what their status measures, and the figures they land on.
  // Recorded rather than described, so a later change that puts the rows back on
  // one grid has to come through this block and section 15h.
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
    // 96px between the home score edges.
    assert(homeNameLefts[0] - homeNameLefts[3] == 48);
    assert(homeScoreRights[0] - homeScoreRights[3] == 96);
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
  // of 59, and 300 gives the ceiling, which is odd at every odd panel width.
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

  std::cout << "PASS: scoreboard columns - the design document's two tables, the measured "
               "status column and its ceiling, what four rows of one board no longer share, "
               "the separation identity at every width up to "
            << kWidestSweep << " over eight status widths, and the floor's boundaries\n";
}
