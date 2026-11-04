#!/usr/bin/env bash
# Holds the lines that decide what a negative score does, in the three files
# that decide it.
#
# TEXT_LAYOUT_DESIGN.md section 15m is the decision: a score below zero is not a
# scoreline, so CheckIn.cpp stores Sports::kNoScore for the whole negative range
# and the two formatters in Sports.cpp write an empty cell for it. What that buys
# the board is an invariant - every score string is decimal digits with no sign -
# and boardScoreState() in Display.cpp rests on it, because that fold keys on the
# length of the string and `-10` is three characters.
#
# tests/card-columns-test.cpp sweeps every value an int16_t holds against
# CardColumns::boardScoreIsDrawable(), and App/CardColumns.h carries static
# assertions over the two boundaries. None of that is reached by any of the three
# call sites: CheckIn.cpp, Sports.cpp and Display.cpp each name a graphics
# library or an Arduino type and none of them compiles on a host, so a predicate
# every one of them stopped calling would go on passing its own test. These are
# the single-token changes that each put the defect back with nothing anywhere
# firing:
#
#   score >= 0 -> score != -1          the shipped defect, exactly: -10 is three
#                                      characters and the board drops a face
#   score >= 0 -> score > 0            a nil-nil draw stops drawing, which is
#                                      the one thing kNoScore exists to keep
#   the ! dropped in writeScore()      every drawable score writes an empty cell
#                                      and the board draws no scores at all
#   the arms of scoreText() swapped    the single-game card draws the sentinel
#   storedScore() off one assignment   one side of every game is unguarded
#   the two fields swapped             home draws away's score
#   return fromWire on the reject path the parse guard does nothing
#   the Log::printf dropped            a dropped score leaves no trace anywhere,
#                                      on the one channel a deployed device has
#
# A text gate rather than a test, in the same spirit as
# ci/check-board-columns-call.sh and ci/check-text-state.sh beside it. It cannot
# see whether the decision is right, only that these four files still make it.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

PREDICATE=App/CardColumns.h
PARSER=App/CheckIn.cpp
FORMATTER=App/Sports.cpp
FOLD=App/Display.cpp
FAIL=0

say() {
  echo "ERROR: $1: $2" >&2
  FAIL=1
}

# A file or a function body read as one run of characters, with the comments and
# then the spacing and the line breaks taken out, so a statement that wraps reads
# the same as one that fits.
#
# **Line comments go first, and that is not tidiness.** Every pattern below is a
# fixed string, and a fixed string cannot tell a statement from a commented-out
# copy of one. Three of these four files are full of remarks that quote code, so
# without this a guard deleted and left behind as `// Was: if (!CardColumns::
# boardScoreIsDrawable(score)) { ... }` satisfies its own check. An adversarial
# pass found exactly that on two of the checks here.
#
# \r as well as space, tab and newline. These files are checked out CRLF under
# core.autocrlf and a carriage return left in the middle of a joined statement
# stops every pattern below from matching, which would make this gate pass by
# finding nothing rather than by checking anything.
#
# Block comments are not stripped. `/* */` appears nowhere in these four files -
# they use `//` and `///` throughout - so a pinned statement hidden in one is a
# gap this does not close. TEST_PLAN.md section 18k records it.
flatten() {
  sed 's://.*::' | tr -d ' \t\r\n'
}

# grep -F, so a pattern carrying the brackets of an array subscript, the `|` of
# an ArduinoJson default or the backslash of an escape is the text it looks like
# rather than a regular expression that has to be escaped twice.
has() {
  printf '%s\n' "$1" | flatten | grep -qF "$2"
}

lacks() {
  ! has "$1" "$2"
}

# How many times a fixed string appears. A `has()` over a whole file finds the
# first occurrence and never looks again, so a second assignment appended after
# the guarded one - what a bad merge leaves behind - passes every check while the
# last write wins. Where a thing has to appear exactly once, this is what says so.
#
# Zero is an answer, so grep's 1 is taken rather than propagated. This script
# runs under pipefail and set -e, and without the `|| true` a count of zero ended
# the script at exit 1 before the comparison below could name which string had
# gone: the figure that says "declared 0 times, not once" is the one worth having.
count() {
  printf '%s\n' "$1" | flatten | { grep -oF "$2" || true; } | wc -l | tr -d ' '
}

read_function() {
  awk -v name="$2" '
    $0 ~ ("^(int16_t|String|void|BoardScores) " name "\\(") { inside = 1 }
    inside                                                  { print }
    inside && /^\}/                                          { exit }
  ' "$1"
}

# --- the predicate itself ----------------------------------------------------
#
# The whole declaration including its body, not the name. Zero draws because a
# nil-nil draw is a scoreline; everything below zero does not, because kNoScore
# is one value of that range and not its boundary. An int parameter rather than
# an int16_t, so a value outside the range is rejected before the narrowing that
# would fold it back in.
PREDICATE_TEXT=$(cat "$PREDICATE")
if lacks "$PREDICATE_TEXT" 'constexprboolboardScoreIsDrawable(intscore){returnscore>=0;}'; then
  say "$PREDICATE" "boardScoreIsDrawable() is not exactly
       'constexpr bool boardScoreIsDrawable(int score) { return score >= 0; }'.
       '> 0' stops a nil-nil draw drawing and '!= -1' puts section 15m's defect back."
fi

# And exactly one of it. A second overload taking int16_t sits beside the pinned
# one, satisfies the check above, and wins overload resolution for both
# formatters, which pass an int16_t - so the pinned text stays in the file while
# the renderers go back to whatever the overload says. The host test passes an
# int and would not see it either.
PREDICATE_COUNT=$(count "$PREDICATE_TEXT" 'boolboardScoreIsDrawable(')
if [ "$PREDICATE_COUNT" != "1" ]; then
  say "$PREDICATE" "boardScoreIsDrawable() is declared $PREDICATE_COUNT times, not once. An
       overload taking int16_t wins for both formatters, which pass one, while the
       pinned declaration above sits untouched."
fi

# The sentinel's own value, in App/Sports.h. It is not one of this gate's four
# files and nothing else reads the line that defines it, so `kNoScore = 0` makes
# every unstarted game store a drawable zero: all four rows of every board draw
# `0`, BoardScores::None becomes TwoDigit, and the score columns take their
# pixels off every name. One token, and the predicate above is still correct.
SENTINEL=App/Sports.h
if [ ! -f "$SENTINEL" ]; then
  say "$SENTINEL" "not found, so the sentinel below checked nothing."
elif lacks "$(cat "$SENTINEL")" 'staticconstexprint16_tkNoScore=-1;'; then
  say "$SENTINEL" "kNoScore is not 'static constexpr int16_t kNoScore = -1;'. A zero there is
       drawable, so every game that has not started draws 0 - 0 on all four rows."
fi

# --- the parse boundary ------------------------------------------------------
if [ ! -f "$PARSER" ]; then
  say "$PARSER" "not found, so the parse guard below checked nothing."
else
  PARSER_TEXT=$(cat "$PARSER")
  STORED=$(read_function "$PARSER" storedScore)

  if [ -z "$STORED" ]; then
    say "$PARSER" "storedScore() not found, so the guard below checked nothing."
  else
    # The signature AND the first statement, as one string.
    #
    # The parameter is in it because `int16_t fromWire` moves the narrowing to
    # the call, ahead of the guard, and the narrowing is modular: -65536 would
    # arrive as 0 and draw a nil, which is the failure this function's own
    # remarks say the int parameter exists to prevent.
    #
    # The guard is the FIRST statement because an `if (fromWire < -1) return 0;`
    # inserted above it leaves every pinned string below present and stores a
    # drawable zero for a -10, logging nothing.
    #
    # Both ends of the range, because the sign alone leaves 65536 folding to a
    # nil-nil draw.
    if lacks "$STORED" 'int16_tstoredScore(intfromWire,constchar*side,constchar*away,constchar*home){if(CardColumns::boardScoreIsDrawable(fromWire)&&fromWire<=INT16_MAX){returnstatic_cast<int16_t>(fromWire);}'; then
      say "$PARSER" "storedScore()'s signature and first statement are not
           'int16_t storedScore(int fromWire, const char* side, const char* away,
           const char* home) { if (CardColumns::boardScoreIsDrawable(fromWire) &&
           fromWire <= INT16_MAX) { return static_cast<int16_t>(fromWire); }'. An int16_t
           parameter narrows before the guard, a statement inserted above it bypasses the
           guard, and dropping the INT16_MAX half lets 65536 fold to a nil-nil draw."
    fi

    # The stream carries what was dropped, and does not carry kNoScore itself,
    # which arrives on nearly every scheduled game.
    #
    # The guard and the call as ONE string, with the call immediately inside the
    # guard's brace. A check that only asked whether `Log::printf(` appears
    # anywhere in the body passed `(void)0; if (false) Log::printf(...)` - the
    # text present, the line unreachable - which this sweep found on its own
    # gate. The message's first two conversions are in the pin because a line
    # that no longer names the side and the number is not the line.
    if lacks "$STORED" 'if(fromWire!=static_cast<int>(Sports::kNoScore)){Log::printf("[sports]%sscore%d'; then
      say "$PARSER" "storedScore()'s log is not a Log::printf() guarded by
           'if (fromWire != static_cast<int>(Sports::kNoScore))' with the call as the first
           statement inside it, naming the side and the score. Without the guard every
           scheduled game logs a line for a score that was never sent; with the call moved
           out from under the brace it can be present and unreachable; and a number the
           card refused is recoverable only off the stream."
    fi

    # The arguments in order, the call closing the guard, and kNoScore returned
    # for everything the guard rejected.
    #
    # The arguments, because swapping `away` and `home` names the wrong teams on
    # the one diagnostic channel a deployed device has, which is the whole reason
    # the check above exists. The return, because a reject path handing back
    # fromWire passes every other check here.
    if lacks "$STORED" ',side,fromWire,static_cast<int>(INT16_MAX),away,home);}returnSports::kNoScore;}'; then
      say "$PARSER" "storedScore() does not end
           '..., side, fromWire, static_cast<int>(INT16_MAX), away, home); } return
           Sports::kNoScore; }'. Swapped team arguments name the wrong game in the stream,
           and what the guard rejects has to reach the renderers as the sentinel they
           already draw an empty cell for."
    fi
  fi

  # Both sides, each reading its own field, with the whole default in the pin and
  # assigned exactly once.
  #
  # The default is in it because `| 0` makes every game that has not started
  # store a drawable zero, which draws 0 - 0 on all four rows and takes the score
  # columns' pixels off every name.
  #
  # Counted because a `has()` over the file stops at the first match: two more
  # unguarded assignments appended after these, which is the shape a bad merge
  # leaves, pass every check while the last write wins.
  for side in home away; do
    PIN="slot.${side}Score=storedScore(game[\"${side}Score\"]|static_cast<int>(Sports::kNoScore),"
    if lacks "$PARSER_TEXT" "$PIN"; then
      say "$PARSER" "slot.${side}Score is not assigned from
           'storedScore(game[\"${side}Score\"] | static_cast<int>(Sports::kNoScore), ...)'. A
           side assigned straight from the wire is unguarded, a side reading the other
           field draws the wrong team's score, and a '| 0' default draws a nil for every
           game that has not started."
    fi
    SIDE_COUNT=$(count "$PARSER_TEXT" "slot.${side}Score=")
    if [ "$SIDE_COUNT" != "1" ]; then
      say "$PARSER" "slot.${side}Score is assigned $SIDE_COUNT times, not once. A second
           assignment after the guarded one leaves every check above satisfied and the
           last write wins."
    fi
  done
fi

# --- the two formatters ------------------------------------------------------
if [ ! -f "$FORMATTER" ]; then
  say "$FORMATTER" "not found, so the two formatters below checked nothing."
else
  WRITE=$(read_function "$FORMATTER" writeScore)
  TEXT=$(read_function "$FORMATTER" scoreText)

  if [ -z "$WRITE" ]; then
    say "$FORMATTER" "writeScore() not found, so the board's score strings below checked
         nothing."
  elif lacks "$WRITE" 'if(!CardColumns::boardScoreIsDrawable(score)){destination[0]='"'"'\0'"'"';return;}'; then
    say "$FORMATTER" "writeScore() does not write an empty cell under
         'if (!CardColumns::boardScoreIsDrawable(score))'. With the ! dropped every
         drawable score writes nothing and the board draws no scores at all; with the
         predicate gone a negative writes a signed string and boardScoreState() reads
         three characters off a board whose largest number is 10."
  fi

  # The format, which is the other half of the board's invariant and was pinned
  # nowhere. `"%+d"` writes `+10` for a 10: three characters, so the board goes
  # to BoardScores::ThreeDigit and drops every score a face, with no negative
  # anywhere. `"%3d"` does the same thing with leading spaces. The capacity is in
  # the pin because the host test's digit sweep formats into the same eight bytes.
  if [ -n "$WRITE" ] &&
     lacks "$WRITE" 'snprintf(destination,capacity,"%d",static_cast<int>(score));'; then
    say "$FORMATTER" "writeScore()'s format is not exactly
         'snprintf(destination, capacity, \"%d\", static_cast<int>(score));'. A sign or a
         width flag writes a three-character string for a two-digit score, which drops
         the board's face without a negative being involved."
  fi

  if [ -z "$TEXT" ]; then
    say "$FORMATTER" "scoreText() not found, so the single-game card's score below checked
         nothing."
  elif lacks "$TEXT" 'returnCardColumns::boardScoreIsDrawable(score)?String(score):String("");'; then
    say "$FORMATTER" "scoreText() is not exactly
         'return CardColumns::boardScoreIsDrawable(score) ? String(score) : String(\"\");'.
         The arms swapped draws the sentinel and blanks every real score."
  fi

  # Neither formatter tests the sentinel on its own. Two rules for one question
  # is how the two answers drift apart, and the predicate is the one rule.
  #
  # Four spellings, because one is one: an adversarial pass wrote the same test
  # as 'kNoScore == score' and as 'score == Sports::kNoScore' and neither was
  # seen. This is still a list of spellings rather than a property, which is what
  # a fixed-string gate can do.
  for body in "$WRITE" "$TEXT"; do
    [ -z "$body" ] && continue
    for spelling in 'score==kNoScore' 'kNoScore==score' 'score==Sports::kNoScore' \
                    'Sports::kNoScore==score'; do
      if has "$body" "$spelling"; then
        say "$FORMATTER" "a formatter still tests '$spelling' beside the predicate.
             One question gets one rule: boardScoreIsDrawable() is it, and a second test
             of the sentinel is how the board and the single-game card come to disagree."
      fi
    done
  done
fi

# --- and the fold stays a question about length ------------------------------
#
# boardScoreState() is held by ci/check-board-columns-call.sh, which pins the
# strlen fold and its return. What belongs here is the other half of section
# 15m's decision: the fold does not know about signs, because the strings it
# reads do not carry one. A sign special-case appearing in it means the guard
# above stopped holding and somebody patched the symptom.
if [ ! -f "$FOLD" ]; then
  say "$FOLD" "not found, so the fold below checked nothing."
else
  STATE=$(read_function "$FOLD" boardScoreState)
  if [ -z "$STATE" ]; then
    say "$FOLD" "boardScoreState() not found, so the fold below checked nothing."
  else
    # The two measurements, whole and with nothing wrapped around them. Pinned
    # this way rather than by forbidding a `'-'`: an adversarial pass wrote the
    # sign case as `- (rows[i].awayScore[0] == 0x2D ? 1u : 0u)` and a check that
    # looked for the character token did not see it, while the statement pin does
    # - and so does any other arithmetic somebody wraps around the length.
    #
    # ci/check-board-columns-call.sh pins the fold's bound, seed and return. This
    # pins the two statements that read the strings, which is the half section
    # 15m's invariant rests on.
    for pin_side in away home; do
      if lacks "$STATE" "constsize_t${pin_side}=strlen(rows[i].${pin_side}Score);"; then
        say "$FOLD" "boardScoreState()'s ${pin_side} measurement is not exactly
             'const size_t ${pin_side} = strlen(rows[i].${pin_side}Score);'. The fold reads
             lengths and the strings carry no sign, because Sports::writeScore() writes
             none. Arithmetic around the length means that guard stopped holding and
             somebody patched the symptom here."
      fi
    done
  fi
fi

if [ "$FAIL" -ne 0 ]; then
  echo "       See TEXT_LAYOUT_DESIGN.md section 15m and the remarks at the top of" >&2
  echo "       ci/check-score-guard.sh." >&2
  exit 1
fi

echo "==> a score below zero reaches the renderers as kNoScore, and both formatters draw it empty"
