#!/usr/bin/env bash
# Holds showSportsBoard()'s call into App/CardColumns.h, and the two folds that
# feed it, to the shape the arithmetic was proven against.
#
# App/CardColumns.h carries seventy-five static_asserts and
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
NOSCORES=$(read_function boardHasNoScores)

if [ -z "$BOARD" ]; then
  say "showSportsBoard() not found, so nothing below checked anything."
  exit 1
fi

if [ -z "$WIDEST" ]; then
  say "widestBoardStatus() not found, so the measurement below checked nothing."
  exit 1
fi

if [ -z "$NOSCORES" ]; then
  say "boardHasNoScores() not found, so the score state below checked nothing."
  exit 1
fi

# A fold's body read as text: the whole of it, rather than the one line that
# names the field. Naming the line and not the loop leaves the bound, the seed
# and the return free, and each of those draws a wrong board on its own - the
# bound reading row 0 only, the seed starting at the ceiling, the return
# throwing the fold away. `tr` first, so a wrapped line reads the same as one
# that fits.
has() {
  printf '%s\n' "$1" | tr -d ' \n' | grep -q "$2"
}

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
#
# drawnTextWidth() rather than lcd.textWidth(), because the two disagree about a
# byte below 0x20 and the panel gets the one the draw path uses.
# LGFXBase::text_width() skips such a byte for no pixels; copySlice() turns it
# into a space, which is 5px at Sans9. A column sized off the raw bytes is
# narrower than the string drawn into it, and the row then ellipsizes a status
# that was measured as fitting. TEXT_LAYOUT_DESIGN.md section 15k.
if ! printf '%s\n' "$WIDEST" | tr -d ' \n' | grep -q 'drawnTextWidth(rows\[i\]\.status)'; then
  say "widestBoardStatus() does not measure drawnTextWidth(rows[i].status). The column holds
       the status as the draw path builds it, and lcd.textWidth() charges nothing for a
       control byte that copySlice() then draws as a space."
fi

# And drawnTextWidth() is that substitution rather than a second name for the raw
# measurement. Two checks, because a body that copies nothing and returns
# lcd.textWidth(text) satisfies the call site above and is this defect with a
# longer name on it. It has to build its copy with copySlice(), which is the one
# function layoutText() draws through, and it has to measure the copy rather
# than its argument. Its own loop over isBreakSpace() would pass a second copy
# of the rule, which is the shape the defect had.
DRAWN=$(read_function drawnTextWidth)
if [ -z "$DRAWN" ]; then
  say "drawnTextWidth() not found, so the measurement above cannot be the drawn one."
else
  if ! has "$DRAWN" 'copySlice(buffer,sizeof(buffer),text,0,length)'; then
    say "drawnTextWidth() does not build its copy with copySlice(buffer, sizeof(buffer), text,
       0, length). copySlice() is the substitution layoutText() draws through, and measuring
       any other copy is a second rule to keep in step with it."
  fi
  if ! has "$DRAWN" 'returnlcd.textWidth(buffer);'; then
    say "drawnTextWidth() does not return lcd.textWidth(buffer). Measuring the argument
       instead of the copy it just made is the raw width under a new name."
  fi
fi

# And the fold keeps the widest of them, over every row, from nothing, and hands
# back what it found. Narrowest is a board sized against a string it does not
# hold; `i < 1` is a board sized on row 0; a seed above zero is a board pinned
# at whatever the seed asks for; returning anything else throws the fold away.
# The host test reads App/CardColumns.h and can see none of the four.
if ! has "$WIDEST" 'if(measured>widest)'; then
  say "widestBoardStatus() does not keep the larger measurement. Every row of the board draws
       into one column, so the column is sized against the widest string on the card."
fi

if ! has "$WIDEST" 'for(uint8_ti=0;i<count;++i)'; then
  say "widestBoardStatus() does not fold over every row. The column the board draws is the one
       its widest string asks for, which needs all of them read."
fi

if ! has "$WIDEST" 'intwidest=0;' || ! has "$WIDEST" 'returnwidest;'; then
  say "widestBoardStatus() does not start at 0 and return what it found. A seed above zero
       pins every board at that width and a different return throws the fold away."
fi

# No second branch. The three checks above all pass on a fold that writes
# `widest` in an else as well, which keeps the last row's measurement with the
# `>` still sitting there in the text. A four-line fold has one branch, so
# saying there is no other is cheaper than reading the shape of this one.
if has "$WIDEST" 'else'; then
  say "widestBoardStatus() has a branch other than the one keeping the larger measurement.
       An else that also writes widest keeps the last row's string with the > still in place."
fi

# The board's score state, the same way. Its body is read by nothing else: no
# host test compiles App/Display.cpp, and four single-token changes to it each
# release both 26px score columns on a board that is drawing digits.
#
# The predicate is `||` and not `&&`, which is the rule App/CardColumns.h states
# in prose beside boardScoreWidth(): a game that reported one side's score is
# not scoreless.
if ! has "$NOSCORES" "rows\[i\]\.awayScore\[0\]!='\\\\0'||rows\[i\]\.homeScore\[0\]!='\\\\0'"; then
  say "boardHasNoScores() does not test both score strings with ||. A row that reported one
       side is not scoreless, so one of them being non-empty is what answers the question."
fi

if ! has "$NOSCORES" 'for(uint8_ti=0;i<count;++i)'; then
  say "boardHasNoScores() does not read every row. One game under way gives the whole board a
       score column, so a row the fold skips is a column the board does not draw."
fi

if ! has "$NOSCORES" 'returnfalse;' || ! has "$NOSCORES" 'returntrue;'; then
  say "boardHasNoScores() does not return false on a row with a score and true at the end.
       The polarity is what picks the score columns, and nothing else reads this function."
fi

# The two-line row, TEXT_LAYOUT_DESIGN.md section 15j. The same problem as
# everything above: App/CardColumns.h holds the arithmetic and reads none of
# the arguments, so these single-token changes each draw a wrong board with
# nothing anywhere firing.
#
#   the two grid calls swapped        a profile panel on the one-line grid and a
#                                     landscape one on the two-line grid
#   kScreenH > kScreenW reversed      two 22px lines into a 35px row
#   the cardHeight term dropped       two lines into a card that cannot hold them
#   homeTextY = awayTextY             both team names on one line, overdrawn
#   the score faces swapped           29px of digits into a 22px line
#   statusY = awayTextY               one value for the pair sitting on the away team
#   the drawRowBrace() call deleted   two lines with nothing saying they are a pair

# Which grid, and the orientation that picks it. The two-line form only where
# two lines fit the card: the height term is what stops a shorter panel drawing
# them through each other.
if ! has "$BOARD" 'stacked=kScreenH>kScreenW&&cardHeight>=2\*lineHeight'; then
  say "'stacked' is not 'kScreenH > kScreenW && cardHeight >= 2 * lineHeight'. The two-line row
       is drawn on a profile panel and only where two lines fit the card."
fi

if ! has "$BOARD" 'stacked?boardStackedColumnsForBoard(kScreenW,scoreless,widestBoardStatus(rows,count))'; then
  say "the two-line grid is not boardStackedColumnsForBoard(kScreenW, scoreless,
       widestBoardStatus(rows, count)) on the stacked branch. Swapped with the one-line call it
       lays a profile panel out for two name columns and a landscape one for a brace."
fi

# The line height the two lines, the brace and the status are all placed from,
# read off the face they draw in rather than written as 22.
if ! printf '%s\n' "$BOARD" | awk '
  /^[[:space:]]*(\/\/|$)/                                        { next }
  /lineHeight = lcd\.fontHeight\(\);/ && previous ~ /lcd\.setFont\(FontId::Sans9\);/ { found = 1 }
  { previous = $0 }
  END                                                            { exit found ? 0 : 1 }
'; then
  say "lineHeight is not lcd.fontHeight() taken on the line after lcd.setFont(FontId::Sans9);.
       Both lines, the brace and the status are placed from it, and fontHeight() reads whichever
       face is current."
fi

# A line apart, and the second one below the first.
if ! has "$BOARD" 'homeTextY=stacked?awayTextY+lineHeight:text'; then
  say "homeTextY is not 'stacked ? awayTextY + lineHeight : text'. The home team is a line below
       the away team in the two-line form and on the same line in the one-line form."
fi

# The status between the two lines, because it describes the pair.
if ! has "$BOARD" 'statusY=stacked?awayTextY+lineHeight/2:text'; then
  say "statusY is not 'stacked ? awayTextY + lineHeight / 2 : text'. FINAL, a period and a start
       time are one value for the game, so the status sits between the two lines."
fi

# The face each score draws in. Two Sans12 slots are 58px against a 52px card,
# so a stacked score draws with its name.
if ! has "$BOARD" 'scoreFont=stacked?FontId::Sans9:FontId::Sans12'; then
  say "scoreFont is not 'stacked ? FontId::Sans9 : FontId::Sans12'. Sans12 is 29px and two of
       them do not fit the profile card, so a two-line score draws in its name's face."
fi

if ! printf '%s\n' "$BOARD" | awk '
  /^[[:space:]]*(\/\/|$)/                                 { next }
  /layoutLine\(row\.awayScore,/ && previous ~ /lcd\.setFont\(scoreFont\);/ { found = 1 }
  { previous = $0 }
  END                                                     { exit found ? 0 : 1 }
'; then
  say "the line before showSportsBoard()'s 'layoutLine(row.awayScore,' is not
       lcd.setFont(scoreFont);. The score is laid out for one face and has to be drawn in it."
fi

# And the brace, spanning both lines, only on a row that has two of them.
if ! has "$BOARD" 'if(stacked){drawRowBrace(boardStackedBraceLeft(),awayTextY,homeTextY+lineHeight-1,'; then
  say "the brace is not drawRowBrace(boardStackedBraceLeft(), awayTextY, homeTextY + lineHeight - 1, ..)
       under 'if (stacked)'. It stands at the row's own left edge and spans the top of the first
       line to the bottom of the second, which is what says the two are one game."
fi

BRACE=$(read_function drawRowBrace)
if [ -z "$BRACE" ]; then
  say "drawRowBrace() not found, so the brace below checked nothing."
elif ! has "$BRACE" 'static_assert(kNoseDepth+kTipLength+1==kBoardBraceWidth'; then
  say "drawRowBrace() does not assert its nose, spine and tip add up to kBoardBraceWidth. The
       column arithmetic reserves that width and nothing else measures what is drawn into it."
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

# The score columns come from the board's grid, not from the constant. A 28px
# column drawn into a block laid out for none is painted through a team name.
#
# Comments are stripped first, because the line that explains why the constant
# is not read here names the constant.
if printf '%s\n' "$BOARD" | sed 's|//.*$||' | grep -q 'kBoardScoreWidth'; then
  say "showSportsBoard() reads kBoardScoreWidth. The score columns are columns.scoreWidth,
       which is 0 on a board where no row reported a score; the constant is 28 always."
fi

# And each of the five fields draws into its own column. Working out the grid
# correctly and then drawing the home name at the away name's edge, or the
# status into the name's width, is a wrong row that every other check here
# passes: they all read how `columns` is built and none read how it is spent.
for pair in \
  'row.awayName,columns.awayNameLeft,awayTextY,columns.nameWidth,:the away name' \
  'row.homeName,columns.homeNameLeft,homeTextY,columns.nameWidth,:the home name' \
  'row.awayScore,columns.awayScoreRight,awayScoreY,columns.scoreWidth,:the away score' \
  'row.homeScore,columns.homeScoreRight,homeScoreY,columns.scoreWidth,:the home score' \
  'row.status,columns.statusRight,statusY,columns.statusWidth,:the status'; do
  want=${pair%%:*}
  what=${pair##*:}
  if ! has "$BOARD" "layoutLine($(printf '%s' "$want" | sed 's/[][\.*^$]/\\&/g')"; then
    say "$what is not drawn at its own edge and its own width. The grid is worked out once and
       spent five times, and a field drawn into another field's column is a wrong row with
       every figure in App/CardColumns.h correct."
  fi
done

# The face the status is DRAWN in, which is the other half of measuring it in
# the face it draws in. The check further up reads the setFont before the
# measurement; measured in Sans9 and drawn in Sans12 is exactly the failure the
# remarks at the top of this file say it exists to prevent, and until this it
# was not read.
if ! printf '%s\n' "$BOARD" | awk '
  /^[[:space:]]*(\/\/|$)/                   { next }
  /layoutLine\(row\.status,/ && previous ~ /lcd\.setFont\(FontId::Sans9\);/ { found = 1 }
  { previous = $0 }
  END                                       { exit found ? 0 : 1 }
'; then
  say "the line before showSportsBoard()'s 'layoutLine(row.status,' is not
       lcd.setFont(FontId::Sans9);. The status is measured in Sans9 and has to be drawn in it,
       or the column holds a string of a different width than the one it was sized for."
fi

# The fitting ladder's own overflow test, which is the other caller that was
# reading raw bytes. layoutLine() decides whether to try a smaller face by
# comparing a measurement against the box; on the raw width it finds a string
# with a control byte in it inside a box that layoutText() then ellipsizes,
# so the ladder is skipped for exactly the strings that need it.
#
# Read off the file rather than through read_function(), which matches a leading
# int, bool or void and layoutLine() returns a TextResult. Grepping the whole
# file is enough here because `overflows` is declared once.
if ! grep -q 'drawnTextWidth(text) > width' "$TARGET"; then
  say "layoutLine()'s overflow test is not drawnTextWidth(text) > width. The ladder has to
       compare the width the string is drawn at, or it skips a tier for the one class of
       string that needs one. TEXT_LAYOUT_DESIGN.md section 15k."
fi

if [ "$(grep -c 'bool overflows =' "$TARGET")" != "1" ]; then
  say "App/Display.cpp declares 'bool overflows =' other than once, so the check above may be
       reading a different one than layoutLine() uses."
fi

# The chevrons sit the same distance in from each edge. The static_assert in
# App/Display.cpp bounds the inset from above, which is what keeps the body off
# the card; it says nothing about the two tips agreeing, so a right tip written
# without the -1 is a pixel nearer the glass than the left one with nothing
# firing.
NAV=$(read_function drawNavAffordances)
if [ -z "$NAV" ]; then
  say "drawNavAffordances() not found, so the chevron tips below checked nothing."
elif ! has "$NAV" 'leftTipX=kChevronInset;' ||
     ! has "$NAV" 'rightTipX=kScreenW-1-kChevronInset;'; then
  say "the chevron tips are not kChevronInset and kScreenW - 1 - kChevronInset. Both bodies
       stand in the margin at the same distance from the glass, and the -1 is what makes the
       right one symmetric with the left."
fi

if [ "$FAIL" -ne 0 ]; then
  echo "       See TEXT_LAYOUT_DESIGN.md sections 15h and 15i and the remarks at the top of" >&2
  echo "       ci/check-board-columns-call.sh." >&2
  exit 1
fi

echo "==> showSportsBoard() lays one grid for the board, measured in the face it draws in"
