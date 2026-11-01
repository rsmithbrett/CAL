#!/usr/bin/env bash
# Holds showSportsBoard()'s call into App/CardColumns.h, and the two folds that
# feed it, to the shape the arithmetic was proven against.
#
# App/CardColumns.h carries forty-one static_asserts and
# tests/card-columns-test.cpp sweeps every panel width from 28 to 1024 twice,
# and between them no single-token change to that header moves a drawn pixel
# without something firing. None of it reads App/Display.cpp. The arguments the
# draw path hands those functions are therefore the one part of this layout
# with no automated check at all, and these single-token changes to them each
# draw a visibly wrong board with nothing anywhere firing:
#
#   boardColumnsForBoard(kScreenW, !scoreless, ..)  score digits through a name
#   kScreenW -> kScreenH                         the board laid out for the wrong
#                                                panel, off the glass in profile
#   rows[i].status -> rows[i].awayName           a 132px status column
#   FontId::Sans9 -> Sans12, or the setFont deleted   measured in the wrong face
#   measured > widest -> measured < widest       the board sized against a string
#                                                it does not hold
#   the columns declared inside the row loop     four rows on four grids, which
#                                                is the defect section 15i fixes
#   the folds moved above the kMaxRows clamp     a fifth row widens a column
#                                                nothing draws into
#
# So this reads those lines and says what they have to be. A text gate rather
# than a test, in the same spirit as check-text-state.sh beside it: it does not
# know what the panel looks like, it knows that the board's status column is
# measured in the face it is drawn in, off the widest string the board holds,
# for the panel it is on, once for the whole card.
#
# TEXT_LAYOUT_DESIGN.md sections 15h and 15i are the design and TEST_PLAN.md
# sections 18e and 18f are what this covers and what it still does not. It
# cannot see whether the measurement is correct, only that the arguments are the
# ones the assertions were written against.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

TARGET=App/Display.cpp
FAIL=0

say() {
  echo "ERROR: $TARGET: $1" >&2
  FAIL=1
}

read_function() {
  awk -v name="$1" '
    $0 ~ ("^(int|bool|void) " name "\\(") { inside = 1 }
    inside                                { print }
    inside && /^\}/                       { exit }
  ' "$TARGET"
}

BOARD=$(read_function showSportsBoard)
WIDEST=$(read_function widestBoardStatus)

if [ -z "$BOARD" ]; then
  say "showSportsBoard() not found, so nothing below checked anything."
  exit 1
fi

if [ -z "$WIDEST" ]; then
  say "widestBoardStatus() not found, so the measurement below checked nothing."
  exit 1
fi

# The face, selected on the line before the fold that measures, so the
# measurement cannot be taken under whatever the previous card left behind.
#
# The line BEFORE, not anywhere in the function: the status is drawn further
# down under its own setFont(FontId::Sans9), so a check that only asks whether
# the file mentions Sans9 passes with the measurement taken at Sans12 and passes
# again with the line deleted. Comments and blank lines are skipped, so a remark
# may sit between the two.
if ! printf '%s\n' "$WIDEST" | awk '
  /^[[:space:]]*(\/\/|$)/                     { next }
  /for \(uint8_t i = 0/ && previous ~ /lcd\.setFont\(FontId::Sans9\);/ { found = 1 }
  { previous = $0 }
  END                                         { exit found ? 0 : 1 }
'; then
  say "the line before widestBoardStatus()'s loop is not lcd.setFont(FontId::Sans9);.
       The status column is measured in the face it draws in, and textWidth() reads
       whichever face is current."
fi

# The measurement: each row's own status string, and nothing else on the row.
if ! printf '%s\n' "$WIDEST" | tr -d ' \n' | grep -q 'lcd\.textWidth(rows\[i\]\.status)'; then
  say "widestBoardStatus() does not measure lcd.textWidth(rows[i].status). The column holds
       the status, so the status is what it is sized against."
fi

# And the fold keeps the widest of them. Narrowest is a board sized against a
# string it does not hold, which ellipsizes every other row's status; the host
# test reads App/CardColumns.h and cannot see which way this comparison runs.
if ! printf '%s\n' "$WIDEST" | tr -d ' \n' | grep -q 'if(measured>widest)'; then
  say "widestBoardStatus() does not keep the larger measurement. Every row of the board draws
       into one column, so the column is sized against the widest string on the card."
fi

# The layout: the panel it is on, the board's own score state, and the widest
# status the board holds, in one call.
if ! printf '%s\n' "$BOARD" | tr -d ' \n' |
     grep -q 'boardColumnsForBoard(kScreenW,scoreless,widestBoardStatus(rows,count))'; then
  say "the columns are not boardColumnsForBoard(kScreenW, scoreless, widestBoardStatus(rows, count)).
       Those three arguments are what App/CardColumns.h's assertions were written against:
       the panel it is on, whether any row of the board reported a score, and the widest
       status string on it."
fi

if ! printf '%s\n' "$BOARD" | tr -d ' \n' | grep -q 'scoreless=boardHasNoScores(rows,count)'; then
  say "scoreless is not boardHasNoScores(rows, count). It is the board's question now and not
       a row's: one game under way gives every row of the card a score column."
fi

# One grid for the card, which is what section 15i is about. Function scope is
# two spaces of indentation and the row loop's body is four, so the declaration
# having moved back inside the loop is a thing this can read.
if ! printf '%s\n' "$BOARD" | grep -qE '^  const BoardColumns columns ='; then
  say "'const BoardColumns columns =' is not declared at showSportsBoard()'s own scope.
       Inside the row loop it is a grid per row, which puts the home name and both score
       columns in a different place on every row of one card."
fi

# And the folds run over the rows that will be drawn. Above the clamp, a fifth
# row the card drops still widens the status column it paid for.
if ! printf '%s\n' "$BOARD" | awk '
  /count = kMaxRows;/                        { clamped = 1 }
  /const BoardColumns columns =/ && !seen    { seen = 1; ordered = clamped }
  END                                        { exit ordered ? 0 : 1 }
'; then
  say "the columns are worked out before 'count = kMaxRows;' clamps the row count. The folds
       read the rows that will be drawn, so a fifth row the card drops cannot widen a column."
fi

# The score columns come from the board's grid, not from the constant. A 26px
# column drawn into a block laid out for none is painted through a team name.
#
# Comments are stripped first, because the line that explains why the constant
# is not read here names the constant.
if printf '%s\n' "$BOARD" | sed 's|//.*$||' | grep -q 'kBoardScoreWidth'; then
  say "showSportsBoard() reads kBoardScoreWidth. The score columns are columns.scoreWidth,
       which is 0 on a board where no row reported a score; the constant is 26 always."
fi

if [ "$FAIL" -ne 0 ]; then
  echo "       See TEXT_LAYOUT_DESIGN.md sections 15h and 15i and the remarks at the top of" >&2
  echo "       ci/check-board-columns-call.sh." >&2
  exit 1
fi

echo "==> showSportsBoard() lays one grid for the board, measured in the face it draws in"
