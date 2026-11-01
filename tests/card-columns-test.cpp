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
//   - Whether the figures this repository has measured for a string fit the
//     column drawn for that string. A static_assert reads 72px against a
//     column of 80 and passes a column of 73 just as happily.
//
// TEST_PLAN.md section 18d has the mutations each case was proven against.

#include <cassert>
#include <iostream>

#include "CardColumns.h"

using CardColumns::boardColumns;
using CardColumns::BoardColumns;
using CardColumns::boardSeparation;
using CardColumns::boardStatusLeft;
using CardColumns::kBoardColumnGap;
using CardColumns::kBoardMinNameWidth;
using CardColumns::kBoardRowInset;
using CardColumns::kBoardScoreWidth;
using CardColumns::kBoardStartTimeSlack;
using CardColumns::kBoardStartTimeTextWidth;
using CardColumns::kBoardStatusWidth;
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

  // --- what a column has to hold --------------------------------------------
  //
  // A scoreless row's status is a start time, measured in this repository at
  // about 72px of FreeSansBold9pt7b, and the column carries the slack on that
  // approximation as well. Reading both names is what separates a column that
  // has been trimmed towards the measurement from one that still has its slack:
  // 73 clears the header's 72px assertion and fails here.
  for (const int width : {320, 240}) {
    assert(boardColumns(width, true).statusWidth >=
           kBoardStartTimeTextWidth + kBoardStartTimeSlack);
    // A row with scores draws FINAL or a period there, which is the 56 the
    // column has always been.
    assert(boardColumns(width, false).statusWidth == kBoardStatusWidth);
    // And two digits of score.
    assert(boardColumns(width, false).scoreWidth == kBoardScoreWidth);
    assert(boardColumns(width, true).scoreWidth == 0);
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
  // from it carry on shrinking, so the home block walks into the status column.
  // The design document names 174 and 202, which is where the separation stops
  // being 2 * kBoardColumnGap. The first width whose name column is clear of the
  // floor is two wider in each case, because the leftover pixels are split with
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

  std::cout << "PASS: scoreboard columns - the design document's table, the separation "
               "identity at every width up to "
            << kWidestSweep << ", the floor's boundaries and the start time's slack\n";
}
