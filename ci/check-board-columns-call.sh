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
#   BoardScores::TwoDigit for a board carrying a 100  an ellipsis for a score
#   BoardScores::ThreeDigit on every board         14px off every name for nothing
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
    $0 ~ ("^(int|bool|void|BoardScores) " name "\\(") { inside = 1 }
    inside                                            { print }
    inside && /^\}/                                   { exit }
  ' "$TARGET"
}

BOARD=$(read_function showSportsBoard)
WIDEST=$(read_function widestBoardStatus)
SCORESTATE=$(read_function boardScoreState)

if [ -z "$BOARD" ]; then
  say "showSportsBoard() not found, so nothing below checked anything."
  exit 1
fi

if [ -z "$WIDEST" ]; then
  say "widestBoardStatus() not found, so the measurement below checked nothing."
  exit 1
fi

if [ -z "$SCORESTATE" ]; then
  say "boardScoreState() not found, so the score state below checked nothing."
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
# The whole statement including its semicolon, not the call as a substring. A
# substring match passes anything wrapped around the call - `drawnTextWidth(..)
# / 2` halves every status column on every board and satisfies a grep for the
# call - and an adversarial pass found exactly that.
if ! has "$WIDEST" 'constintmeasured=drawnTextWidth(rows\[i\].status);'; then
  say "widestBoardStatus()'s measurement is not exactly
       'const int measured = drawnTextWidth(rows[i].status);'. The column holds the status as
       the draw path builds it, and arithmetic wrapped around the call is a column the
       arithmetic in App/CardColumns.h was never proven against."
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
  # The guard with its `!`, not the call. Dropping one character puts the raw
  # measurement on the normal path and the copy on the long one, and a grep for
  # the copySlice() call alone passes it. An adversarial pass found that too.
  if ! has "$DRAWN" 'if(!copySlice(buffer,sizeof(buffer),text,from,to)){'; then
    say "drawnTextWidth() does not guard on '!copySlice(buffer, sizeof(buffer), text, from,
       to)'. copySlice() is the substitution layoutText() draws through, the slice is the
       string with its leading and trailing break space off, and a dropped ! returns the raw
       width on every string that fits the buffer."
  fi
  if ! has "$DRAWN" 'returnlcd.textWidth(buffer);'; then
    say "drawnTextWidth() does not return lcd.textWidth(buffer). Measuring the argument
       instead of the copy it just made is the raw width under a new name."
  fi
  # The length, because sizeof(text) on a pointer measures the first four bytes
  # of every string and satisfies both checks above.
  if ! has "$DRAWN" 'size_tto=strlen(text);'; then
    say "drawnTextWidth() does not take its end from strlen(text). sizeof() on the pointer
       measures four bytes of every string."
  fi
  # Both ends trimmed, which is what layoutText() drops. Without these a
  # trailing control byte is charged a space advance it never draws.
  if ! has "$DRAWN" 'while(from<to&&isBreakSpace(text\[from\])){'; then
    say "drawnTextWidth() does not strip leading break space. layoutText() skips it rather
       than drawing it, so measuring it charges pixels nothing puts on the panel."
  fi
  if ! has "$DRAWN" 'while(to>from&&isBreakSpace(text\[to-1\])){'; then
    say "drawnTextWidth() does not strip trailing break space. A line ends at a word
       boundary, so trailing break space is never drawn and must not be measured."
  fi
fi

# The two primitives drawnTextWidth() now depends on, which nothing read before
# it did. isBreakSpace() decides what a control byte is for the wrap, for the
# substitution and now for the measurement, and copySlice()'s substitution is
# the rule the measurement is supposed to agree with. Single-token changes to
# either put the measure path and the draw path back out of step with nothing
# firing, which is the defect TEXT_LAYOUT_DESIGN.md section 15k is about.
BREAKSPACE=$(read_function isBreakSpace)
if [ -z "$BREAKSPACE" ]; then
  say "isBreakSpace() not found, so the break-space rule below checked nothing."
elif ! has "$BREAKSPACE" 'returnstatic_cast<unsignedchar>(c)<=0x20;'; then
  say "isBreakSpace() is not 'static_cast<unsigned char>(c) <= 0x20'. At < 0x20 a space stops
       being a break opportunity and nothing in the binary wraps; wider, printable bytes are
       drawn as spaces."
fi

SLICE=$(read_function copySlice)
if [ -z "$SLICE" ]; then
  say "copySlice() not found, so the substitution below checked nothing."
else
  # Matched on the raw text rather than through has(), which strips spaces and
  # would read the ' ' literal as ''. The space is the whole substitution.
  if ! printf '%s\n' "$SLICE" | grep -qF "buffer[i] = isBreakSpace(c) ? ' ' : c;"; then
    say "copySlice() does not substitute a space for a break-space byte. That substitution is
       what drawnTextWidth() measures and what the panel draws, and the two have to be the
       same rule."
  fi
  if ! has "$SLICE" 'if(length+1>capacity){'; then
    say "copySlice() does not check 'length + 1 > capacity'. The terminator it writes at
       buffer[length] needs the +1, and without it a slice of exactly capacity bytes writes
       one past the end."
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
# release both 28px score columns on a board that is drawing digits.
#
# Both score strings are read, not one. A fold reading only the away side gives
# a board whose three-digit score is on the home side the wrong state, which is
# the rule App/CardColumns.h states in prose beside boardScoreWidth(): a game
# that reported one side's score is not BoardScores::None.
if ! has "$SCORESTATE" 'strlen(rows\[i\].awayScore)' ||
   ! has "$SCORESTATE" 'strlen(rows\[i\].homeScore)'; then
  say "boardScoreState() does not measure both score strings with strlen(). A row that
       reported one side is not BoardScores::None, and the longest string on the board is
       what the column has to hold."
fi

if ! has "$SCORESTATE" 'for(uint8_ti=0;i<count;++i)'; then
  say "boardScoreState() does not read every row. One game under way gives the whole board a
       score column and one score over 99 gives it the wider one, so a row the fold skips is
       a column the board draws wrong."
fi

# The three answers, and the boundary between the last two. `>= 3` and not `> 3`
# is what makes a three-character score reach the wider column; at `> 3` a 100
# gets the two-digit column and draws an ellipsis, which is the defect section
# 15l closed.
if ! has "$SCORESTATE" 'if(longest==0){' || ! has "$SCORESTATE" 'returnBoardScores::None;'; then
  say "boardScoreState() does not return BoardScores::None for a board whose score strings are
       all empty. That is what releases both score columns."
fi

if ! has "$SCORESTATE" 'returnlongest>=3?BoardScores::ThreeDigit:BoardScores::TwoDigit;'; then
  say "boardScoreState() does not return 'longest >= 3 ? BoardScores::ThreeDigit :
       BoardScores::TwoDigit'. Three characters is where the wider column starts; at > 3 a
       100 draws an ellipsis, and at >= 2 every board pays for a column it does not need."
fi

# And it keeps the longest rather than the last. A fold that assigns without
# comparing passes every check above and reports the final row's score.
if ! has "$SCORESTATE" 'if(away>longest){' || ! has "$SCORESTATE" 'if(home>longest){'; then
  say "boardScoreState() does not keep the longer of each pair. Assigning without comparing
       reports the last row's score rather than the widest on the board."
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

if ! has "$BOARD" 'stacked?boardStackedColumnsForBoard(kScreenW,scores,widestBoardStatus(rows,count))'; then
  say "the two-line grid is not boardStackedColumnsForBoard(kScreenW, scores,
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

# The face each score draws in, and the one decision that picks it. Two Sans12
# slots are 58px against a 52px card, so a stacked score draws with its name;
# three digits of Sans12 are 42px and would cost each name 14, so a three-digit
# board draws its scores at Sans9 too. TEXT_LAYOUT_DESIGN.md section 15l.
#
# `bigScores` is read by the face and by the 2px lift that centres Sans12 on
# Sans9, so one expression decides both and a board that drops the face cannot
# keep the lift. The whole declaration, because `!stacked && scores ==` flipped
# to `||` or to `!=` draws a three-digit board at Sans12 through its own names
# and every check below still passes.
if ! has "$BOARD" 'constboolbigScores=!stacked&&scores!=BoardScores::ThreeDigit;'; then
  say "bigScores is not '!stacked && scores != BoardScores::ThreeDigit'. Sans12 is 29px and
       two of them do not fit the profile card, and three digits of it are 42px against a
       30px column, so both of those boards draw their scores at Sans9."
fi

if ! has "$BOARD" 'scoreFont=bigScores?FontId::Sans12:FontId::Sans9'; then
  say "scoreFont is not 'bigScores ? FontId::Sans12 : FontId::Sans9'. The face is the board's
       one decision, and reading anything else here lets the column and the face disagree."
fi

# The lift reads the same thing. It centres a 29px face on a 22px line and is
# wrong by 2px wherever the score is already Sans9.
if ! has "$BOARD" 'awayScoreY=stacked?awayTextY:(bigScores?text-2:text)' ||
   ! has "$BOARD" 'homeScoreY=stacked?homeTextY:(bigScores?text-2:text)'; then
  say "the score baselines are not 'stacked ? ...TextY : (bigScores ? text - 2 : text)'. The
       2px lift centres Sans12 on a Sans9 line, so a board drawing Sans9 scores must not
       take it."
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
     grep -q 'boardColumnsForBoard(kScreenW,scores,widestBoardStatus(rows,count))'; then
  say "the columns are not boardColumnsForBoard(kScreenW, scores, widestBoardStatus(rows, count)).
       Those three arguments are what App/CardColumns.h's assertions were written against:
       the panel it is on, what the widest score on it needs, and the widest
       status string on it."
fi

if ! printf '%s\n' "$BOARD" | tr -d ' \n' | grep -q 'scores=boardScoreState(rows,count)'; then
  say "scores is not boardScoreState(rows, count). It is the board's question and not
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
# The whole predicate, not the comparison. `text[0] != '\0'` flipped to `==`
# leaves this grep matching and turns the ladder off for every real string,
# which is the mechanism the score column half of section 15k depends on.
if ! printf '%s\n' "$(cat "$TARGET")" | tr -d ' \n' |
     grep -qF "width>0&&text!=nullptr&&text[0]!='\0'&&drawnTextWidth(text)>width;"; then
  say "layoutLine()'s overflow test is not
       'width > 0 && text != nullptr && text[0] != '\\0' && drawnTextWidth(text) > width'.
       The ladder has to compare the width the string is drawn at, over every string rather
       than the empty one. TEXT_LAYOUT_DESIGN.md section 15k."
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
