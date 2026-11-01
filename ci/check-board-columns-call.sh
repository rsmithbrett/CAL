#!/usr/bin/env bash
# Holds showSportsBoard()'s two calls into App/CardColumns.h to the shape the
# arithmetic was proven against.
#
# App/CardColumns.h carries thirty-two static_asserts and
# tests/card-columns-test.cpp sweeps every panel width from 28 to 1024 twice,
# and between them no single-token change to that header moves a drawn pixel
# without something firing. None of it reads App/Display.cpp. The arguments the
# draw path hands those functions are therefore the one part of this layout
# with no automated check at all, and five single-token changes to them each
# draw a visibly wrong row with nothing anywhere firing:
#
#   boardColumnsFor(kScreenW, !scoreless, ...)   score digits through a name
#   boardStatusWidthFor(kScreenW, !scoreless, ..)  the name floor broken at 240
#   kScreenW -> kScreenH in either call          the row laid out for the wrong
#                                                panel, off the glass in profile
#   textWidth(row.status) -> textWidth(row.awayName)  a 132px status column
#   FontId::Sans9 -> Sans12, or the setFont deleted   measured in the wrong face
#
# So this reads the four lines and says what they have to be. A text gate rather
# than a test, in the same spirit as check-text-state.sh beside it: it does not
# know what the panel looks like, it knows that the status column is measured in
# the face it is drawn in, off the string it holds, for the panel it is on.
#
# TEXT_LAYOUT_DESIGN.md section 15h is the design and TEST_PLAN.md section 18e
# is what this covers and what it still does not. It cannot see whether the
# measurement is correct, only that the four arguments are the four the
# assertions were written against.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

TARGET=App/Display.cpp
FAIL=0

say() {
  echo "ERROR: $TARGET: $1" >&2
  FAIL=1
}

# The block, read as one unit: the face selected, then the measurement, then the
# layout. awk rather than grep because the three are only right in that order,
# and because the measurement call wraps across two lines.
BLOCK=$(awk '
  /^void showSportsBoard\(/ { inside = 1 }
  inside                    { print }
  inside && /^\}/           { exit }
' "$TARGET")

if [ -z "$BLOCK" ]; then
  say "showSportsBoard() not found, so nothing below checked anything."
  exit 1
fi

# The face, selected on the line before the measurement, so the measurement
# cannot be taken under whatever the previous row left behind.
#
# The line BEFORE, not anywhere in the function: the status is drawn further
# down under its own setFont(FontId::Sans9), so a check that only asks whether
# the function mentions Sans9 passes with the measurement taken at Sans12 and
# passes again with the line deleted. Comments and blank lines are skipped, so
# a remark may sit between the two.
if ! printf '%s\n' "$BLOCK" | awk '
  /^[[:space:]]*(\/\/|$)/                           { next }
  /const BoardColumns columns/ && previous ~ /lcd\.setFont\(FontId::Sans9\);/ { found = 1 }
  { previous = $0 }
  END                                               { exit found ? 0 : 1 }
'; then
  say "the line before showSportsBoard()'s 'const BoardColumns columns' is not
       lcd.setFont(FontId::Sans9);. The status column is measured in the face it draws
       in, and textWidth() reads whichever face is current."
fi

# The measurement: the row's own status, for the panel's own width.
if ! printf '%s\n' "$BLOCK" | tr -d ' \n' |
     grep -q 'boardStatusWidthFor(kScreenW,scoreless,lcd\.textWidth(row\.status))'; then
  say "the status width is not boardStatusWidthFor(kScreenW, scoreless, lcd.textWidth(row.status)).
       Those three arguments are what App/CardColumns.h's assertions were written against:
       the panel it is on, the row's own score state, and the string the column holds."
fi

# The layout: the same panel, the same state, and the width just measured rather
# than a second figure.
if ! printf '%s\n' "$BLOCK" | tr -d ' \n' |
     grep -q 'boardColumnsFor(kScreenW,scoreless,boardStatusWidthFor('; then
  say "the columns are not boardColumnsFor(kScreenW, scoreless, boardStatusWidthFor(...)).
       The status width is charged to the row as well as drawn, so the two calls have to
       agree on the panel, on the state and on the width."
fi

# The score columns come from the row, not from the constant. A 26px column
# drawn into a block laid out for none is painted through a team name.
#
# Comments are stripped first, because the line that explains why the constant
# is not read here names the constant.
if printf '%s\n' "$BLOCK" | sed 's|//.*$||' | grep -q 'kBoardScoreWidth'; then
  say "showSportsBoard() reads kBoardScoreWidth. The score columns are columns.scoreWidth,
       which is 0 on a row with no scores; the constant is 26 on every row."
fi

if [ "$FAIL" -ne 0 ]; then
  echo "       See TEXT_LAYOUT_DESIGN.md section 15h and the remarks at the top of" >&2
  echo "       ci/check-board-columns-call.sh." >&2
  exit 1
fi

echo "==> showSportsBoard() measures its status column in the face it draws it in"
