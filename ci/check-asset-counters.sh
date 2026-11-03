#!/usr/bin/env bash
# Holds App/Assets.cpp's two reported counters to the shape
# App/ReportedCounter.h proves.
#
# tests/reported-counter-test.cpp walks the whole range of a uint8_t through
# countOne(), and none of it reads App/Assets.cpp. Whether the firmware's own
# increments go through that function is therefore the part with no automated
# check at all, and a bare ++ on either counter compiles, links, passes every
# host test and reports a wrapped figure to the server:
#
#   gDecodeFailureCount   rides the check-in request as assetDecodeFailureCount.
#                         Wrapped, it reads as fewer failures than occurred, and
#                         at exactly 256 it reads as none - which withholds
#                         assetDecodeFailureIds with it and skips the clear that
#                         CheckIn.cpp performs after a successful round trip, so
#                         the ids pin and the next cycle starts from a count
#                         that is already wrong.
#   gAllocationRefusals   rides the refusal log line. Wrapped, the figure in
#                         that line stops being the number of refusals this
#                         reporting cycle, which is the one thing it says.
#
# A text gate rather than a test, in the same spirit as check-ram-budget-call.sh
# and check-board-columns-call.sh beside it. It does not know whether the
# ceiling is the right ceiling; it knows that neither counter is incremented any
# other way, and that the third counter in the file keeps the cap check that
# makes it the other kind of counter.
#
# TEST_PLAN.md section 21 is what this covers and what it still does not. It
# cannot see what the device reports - only which function each increment runs
# through.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

TARGET=App/Assets.cpp
HEADER=App/ReportedCounter.h
FAIL=0

say() {
  echo "ERROR: $TARGET: $1" >&2
  FAIL=1
}

for f in "$TARGET" "$HEADER"; do
  if [ ! -f "$f" ]; then
    echo "ERROR: $f not found, so nothing below checked anything." >&2
    exit 1
  fi
done

# Code only. The file's own comments name both counters and the function at
# length, so a gate that read them would be satisfied by its own prose: it would
# pass a recordDecodeFailure() whose body had been emptied as long as the
# remarks above it still described what it used to do.
#
# Tab and carriage return go out with the spaces and the newlines, the way
# ci/check-content-key-resolvers.sh flattens: this file is checked out CRLF under
# core.autocrlf, and a carriage return left in the middle of a joined statement
# stops every pattern below from matching, which would make this gate pass by
# finding nothing rather than by checking anything.
CODE=$(sed 's;//.*$;;' "$TARGET")
FLAT=$(printf '%s\n' "$CODE" | tr -d ' \t\r\n')

# grep returning 1 is an answer here, not a failure, and this script runs under
# pipefail - so every match is taken with || true and judged on its output.
has() { printf '%s' "$FLAT" | grep -q "$1"; }
count_matches() { printf '%s' "$FLAT" | grep -o "$1" | wc -l | tr -d ' '; }

# ---------------------------------------------------------------------------
# 1. The rule is included, so the two counters can reach it at all.
# ---------------------------------------------------------------------------
if ! printf '%s\n' "$CODE" | grep -q '#include "ReportedCounter.h"'; then
  say "ReportedCounter.h is not included, so neither counter can be reaching countOne()."
fi

# ---------------------------------------------------------------------------
# 2. Neither reported counter is written anywhere but countOne() and a reset.
#
#    ++x, x--, x += n and a compound assignment of any operator all read as a
#    write here, and so does `x = <anything but 0>`. The ceiling lives in
#    countOne() and nowhere else, so a write beside it is the wrap by another
#    route even when a guard happens to sit above it: a second copy of the
#    ceiling is a second thing to keep right.
#
#    `x = 0` is the one assignment that stays. It is the declaration and the
#    clear CheckIn.cpp performs after a successful round trip, neither of which
#    can overflow. Everything else is counted and compared, because a plain
#    `gAllocationRefusals = gAllocationRefusals + 1;` sitting beside the kept
#    countOne() call reads as a wrap and matched none of the operators above it.
# ---------------------------------------------------------------------------
for counter in gDecodeFailureCount gAllocationRefusals; do
  if printf '%s' "$FLAT" | grep -qE "((\+\+|--)$counter|$counter(\+\+|--))"; then
    say "$counter is incremented or decremented directly. It has to go through ReportedCounter::countOne(), which is the only place the ceiling is written."
  fi

  if printf '%s' "$FLAT" | grep -qE "$counter(\+|-|\*|/|%|&|\||\^|<<|>>)="; then
    say "$counter is written through a compound assignment. It has to go through ReportedCounter::countOne(), which is the only place the ceiling is written."
  fi

  # One character past the `=` so a comparison is not read as a write: `x == 0`
  # flattens to `x==0` and would otherwise count as an assignment.
  WRITES=$(count_matches "$counter=[^=]" || true)
  CLEARS=$(count_matches "$counter=0;" || true)
  if [ "$WRITES" != "$CLEARS" ]; then
    say "$counter is assigned something other than 0 in $(( WRITES - CLEARS )) place(s). The only assignments it may carry are its declaration and the clear after a reported cycle; a figure computed into it is the wrap countOne() exists to hold, whether or not a countOne() call still sits beside it."
  fi

  if ! has "ReportedCounter::countOne($counter)"; then
    say "$counter never reaches ReportedCounter::countOne(), so nothing holds it at the ceiling."
  fi
done

# Every countOne() call in the file names one of the two counters, so a third
# one cannot be added without this gate being told about it.
CALLS=$(count_matches 'ReportedCounter::countOne(' || true)
NAMED=$(count_matches 'ReportedCounter::countOne(\(gDecodeFailureCount\|gAllocationRefusals\))' || true)
if [ "$CALLS" = "0" ]; then
  say "no ReportedCounter::countOne() calls found, so every check above checked nothing."
elif [ "$CALLS" != "$NAMED" ]; then
  say "$CALLS countOne() calls but only $NAMED name a counter this gate knows. Add the new counter to the loop above, or it is checked by nothing."
fi

# ---------------------------------------------------------------------------
# 3. The decode-failure count still counts outside the id-list branch.
#
#    The 4-id cap is a cap on the list, not on the count. Moving the count
#    inside the append branch would pin it at 4 and lose the difference between
#    four bad assets and one card failing all day - the thing the count is for,
#    and a defect no ceiling can see.
# ---------------------------------------------------------------------------
RECORD=$(printf '%s\n' "$CODE" | awk '
  /^void recordDecodeFailure\(/ { inside = 1 }
  inside                        { print }
  inside && /^\}/               { exit }
')
if [ -z "$RECORD" ]; then
  say "recordDecodeFailure() not found, so the count's placement checked nothing."
else
  RECORD_FLAT=$(printf '%s\n' "$RECORD" | tr -d ' \n')
  if ! printf '%s' "$RECORD_FLAT" \
    | grep -q 'kMaxReportedDecodeFailures){.*}ReportedCounter::countOne(gDecodeFailureCount)'; then
    say "the count must run after the kMaxReportedDecodeFailures branch closes, not inside it. Inside, the figure stops at 4 and says nothing the id list does not already say."
  fi
fi

# ---------------------------------------------------------------------------
# 4. The cheap-refetch counter is the other kind, and keeps its cap check.
#
#    It is compared against kMaxCheapRefetches before it is incremented, which
#    is what makes it unable to overflow. Dropping that check would make it a
#    third reported counter with no ceiling, and the fetch budget it enforces
#    would stop being a budget.
# ---------------------------------------------------------------------------
if ! has 'gCheapRefetchCount>=kMaxCheapRefetches'; then
  say "gCheapRefetchCount must be compared against kMaxCheapRefetches before it is incremented. That comparison is both the per-boot fetch budget and the reason this counter needs no ceiling."
fi

# ---------------------------------------------------------------------------
# 5. The ceiling is the field's own limit, read from the header.
# ---------------------------------------------------------------------------
if ! sed 's;//.*$;;' "$HEADER" | tr -d ' \n' | grep -q 'kCeiling=UINT8_MAX'; then
  say "ReportedCounter::kCeiling must be UINT8_MAX. A figure chosen beside the type can be lower than what the field carries, which withholds counts the report has room for."
fi

if [ "$FAIL" = "0" ]; then
  echo "PASS: asset counters - both reported counters go through countOne(), the count stays outside the id-list branch, the refetch budget keeps its cap check"
fi

exit "$FAIL"
