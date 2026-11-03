#!/usr/bin/env bash
# Holds Assets.cpp's calls into App/RamPictureBudget.h to the shape the rules
# were proven against.
#
# tests/ram-picture-budget-test.cpp grades the two rules over four measured
# devices and both build shapes, and none of it reads App/Assets.cpp. The
# arguments the fetch path hands those functions are therefore the part of this
# with no automated check at all, and each of these single-token changes gates a
# picture on the wrong number with nothing anywhere firing:
#
#   largestTlsBlockBytes -> largestAnyBlockBytes   on the Panellan the bare
#                                                  figure is 1,998,836 while the
#                                                  pool a handshake allocates
#                                                  from holds 86,004, so every
#                                                  picture passes and an
#                                                  unreachable device reports
#                                                  room to spare
#   externalRam read from the build, not the heap  a board built with PSRAM that
#                                                  came up without it is graded
#                                                  as though it had some
#   externalOnly dropped from the grow             the picture is cleared against
#                                                  PSRAM and then allocated out
#                                                  of internal DRAM anyway,
#                                                  which is the pool the check
#                                                  had just spared
#   leavesHandshakePossible before the allocation  a measurement turned back into
#                                                  a prediction, which is the
#                                                  rule device 12 disproves
#   the release dropped from the refusal           the buffer the guard refused
#                                                  stays allocated, so the guard
#                                                  costs the heap it was saving
#
# A text gate rather than a test, in the same spirit as check-text-state.sh and
# check-board-columns-call.sh beside it. It does not know whether the figures
# are right; it knows which function each one comes from, that placement is read
# from the heap rather than the build, and that a refusal hands its buffer back.
#
# TEST_PLAN.md section 16j is what this covers and what it still does not. It
# cannot see whether heap_caps_realloc actually placed the bytes in PSRAM - only
# that the flag asking for it is the one derived from the live heap.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

TARGET=App/Assets.cpp
FAIL=0

say() {
  echo "ERROR: $TARGET: $1" >&2
  FAIL=1
}

if [ ! -f "$TARGET" ]; then
  echo "ERROR: $TARGET not found, so nothing below checked anything." >&2
  exit 1
fi

# The function read as text, whitespace squeezed, so a wrapped argument list
# reads the same as one that fits on a line.
read_function() {
  awk -v name="$1" '
    $0 ~ ("^bool " name "\\(")  { inside = 1 }
    inside                      { print }
    inside && /^\}/             { exit }
  ' "$TARGET"
}

FETCH=$(read_function fetchToRamImpl)
if [ -z "$FETCH" ]; then
  say "fetchToRamImpl() not found, so every check below checked nothing."
  exit 1
fi

# CODE ONLY. The // comments go first, before anything is flattened.
#
# This file's own comments name every function and argument below, at length, so
# a gate that read them would be satisfied by its own prose - it would pass a
# fetchToRamImpl() whose body had been emptied, as long as the remarks above it
# still described what it used to do. Stripping comments first is what makes
# this check about the code.
CODE=$(printf '%s\n' "$FETCH" | sed 's;//.*$;;')
FLAT=$(printf '%s\n' "$CODE" | tr -d ' \n')

# grep returning 1 is an answer here, not a failure, and this script runs under
# pipefail - so every match is taken with || true and judged on its output.
has() { printf '%s' "$FLAT" | grep -q "$1"; }
count_matches() { printf '%s' "$FLAT" | grep -o "$1" | wc -l | tr -d ' '; }

# ---------------------------------------------------------------------------
# 1. Both rules are graded against the INTERNAL pool, never the bare figure.
# ---------------------------------------------------------------------------
if ! has 'fitsBesideTls(pictureBytes,HeapPools::largestTlsBlockBytes(),'; then
  say "fitsBesideTls() must take HeapPools::largestTlsBlockBytes() as its pool argument. The bare largestAnyBlockBytes() spans PSRAM on the Panellan and would pass every picture."
fi

if ! has 'leavesHandshakePossible(HeapPools::largestTlsBlockBytes(),'; then
  say "leavesHandshakePossible() must take HeapPools::largestTlsBlockBytes() as its pool argument, for the same reason."
fi

if printf '%s' "$FLAT" | grep -q 'fitsBesideTls(.*largestAnyBlockBytes'; then
  say "fitsBesideTls() is being handed the bare 8-bit figure. See HeapPools.h: that figure describes a device that cannot stand up a handshake as having megabytes free."
fi

if printf '%s' "$FLAT" | grep -q 'leavesHandshakePossible(.*largestAnyBlockBytes'; then
  say "leavesHandshakePossible() is being handed the bare 8-bit figure."
fi

# ---------------------------------------------------------------------------
# 2. Both rules use the build's derived demand, not a hardcoded figure.
# ---------------------------------------------------------------------------
# EVERY call, not just one of them. Checking that kTlsDemand appears somewhere
# passed a mutation that replaced the demand in the fitsBesideTls() call with a
# literal and left the other call alone - the surviving kTlsDemand satisfied the
# check while the rule that gates the house-hunt card was reading a hardcoded
# figure. So the two counts have to agree.
BUDGET_CALLS=$(count_matches 'RamPictureBudget::\(fitsBesideTls\|leavesHandshakePossible\)(' || true)
DEMAND_CALLS=$(count_matches 'RamPictureBudget::\(fitsBesideTls\|leavesHandshakePossible\)([^;]*kTlsDemand' || true)

if [ "$BUDGET_CALLS" = "0" ]; then
  say "no RamPictureBudget:: calls found, so the derived demand checked nothing."
elif [ "$BUDGET_CALLS" != "$DEMAND_CALLS" ]; then
  say "$BUDGET_CALLS budget calls but only $DEMAND_CALLS pass kTlsDemand. Every one has to take the demand derived from this build's own sdkconfig: the two trees differ by 12,288 bytes and a literal is wrong on one of them."
fi

# And no TLS figure written out by hand anywhere in the function. The derived
# constants live above it, where static_asserts check them; inside the fetch
# path a literal is the mutation above by another route.
if printf '%s' "$FLAT" | grep -qE '(16717|21146|33434)'; then
  say "a TLS figure (16717, 21146 or 33434) is written as a literal inside fetchToRamImpl(). It belongs in kTlsDemand, where a static_assert checks it landed on one of the two figures this fleet builds."
fi

# ---------------------------------------------------------------------------
# 3. Placement is read from the live heap, not from the build configuration.
#
#    A board built with PSRAM can come up without it - esp_psram_init() fails
#    and no SPIRAM region is registered - so the only answer that describes the
#    device in front of you is the heap's.
# ---------------------------------------------------------------------------
if ! has 'externalRam=HeapPools::externalRamInHeap()'; then
  say "the placement flag must come from HeapPools::externalRamInHeap(), a runtime heap read. A CONFIG_SPIRAM check would grade a board that came up without its PSRAM as though it had some."
fi

if printf '%s' "$FLAT" | grep -qE 'externalRam=.*(CONFIG_SPIRAM|defined\()'; then
  say "the placement flag is being derived from the build configuration rather than the heap."
fi

# ---------------------------------------------------------------------------
# 4. A budgeted allocation is cap-restricted, so the prediction's premise holds.
#
#    Both grow calls, not just the up-front one: a chunked response reaches the
#    in-loop grow with no Content-Length, and that is then the only thing
#    keeping the bytes out of the pool the check spared.
# ---------------------------------------------------------------------------
if ! has 'externalOnly=budgeted&&externalRam'; then
  say "externalOnly must be (budgeted && externalRam): that is what makes fitsBesideTls()'s placedInExternalRam argument a fact rather than a hope."
fi

# Bounded on ';' rather than ')', because the argument itself carries parentheses
# - ensureRamBufferCapacity(buffer, static_cast<size_t>(expectedSize), externalOnly) -
# and a [^)]* pattern stops inside the cast and matched none of them.
GROWS=$(count_matches 'ensureRamBufferCapacity(buffer,' || true)
GUARDED=$(count_matches 'ensureRamBufferCapacity(buffer,[^;]*externalOnly)' || true)
if [ "$GROWS" = "0" ]; then
  say "no calls to ensureRamBufferCapacity() found, so the placement restriction checked nothing."
elif [ "$GROWS" != "$GUARDED" ]; then
  say "$GROWS calls to ensureRamBufferCapacity() but only $GUARDED pass externalOnly. Every grow on this path has to carry the placement restriction, the in-loop chunked one included."
fi

# ---------------------------------------------------------------------------
# 5. The measurement is taken AFTER the allocation, and a refusal hands the
#    buffer back.
# ---------------------------------------------------------------------------
ALLOC_LINE=$(printf '%s\n' "$CODE" | grep -n 'ensureRamBufferCapacity(buffer' | head -1 | cut -d: -f1 || true)
MEASURE_LINE=$(printf '%s\n' "$CODE" | grep -n 'leavesHandshakePossible' | head -1 | cut -d: -f1 || true)

if [ -z "$ALLOC_LINE" ] || [ -z "$MEASURE_LINE" ]; then
  say "could not locate both the allocation and the leavesHandshakePossible() measurement, so their order checked nothing."
elif [ "$MEASURE_LINE" -lt "$ALLOC_LINE" ]; then
  say "leavesHandshakePossible() is called BEFORE the allocation (line $MEASURE_LINE against $ALLOC_LINE). It is a measurement of the pool after the picture is in it; asked beforehand it is the prediction device 12 disproves."
fi

# The refusal block has to release. Read as the window between the measurement
# and the return that follows it.
REFUSAL=$(printf '%s\n' "$CODE" | awk -v start="${MEASURE_LINE:-0}" 'start>0 && NR>=start && NR<=start+14')
if ! printf '%s' "$REFUSAL" | tr -d ' \n' | grep -q 'releaseRamBuffer(buffer)'; then
  say "the leavesHandshakePossible() refusal must call releaseRamBuffer(buffer). Refusing without releasing leaves the allocation the guard objected to in place, which spends the heap it was protecting."
fi

# ---------------------------------------------------------------------------
# 6. The retrofit applies to the unbudgeted callers only. A budgeted fetch was
#    already cleared against the stricter rule and its bytes are in PSRAM.
# ---------------------------------------------------------------------------
if ! has '!budgeted&&expectedSize>0&&!RamPictureBudget::leavesHandshakePossible'; then
  say "the measurement must be gated on !budgeted. Applied to the house-hunt path as well it would second-guess fitsBesideTls() with a weaker rule."
fi

if [ "$FAIL" = "0" ]; then
  echo "PASS: ram budget call site - both pools internal, demand derived, placement read from the heap, measurement after the allocation, refusal releases"
fi

exit "$FAIL"
