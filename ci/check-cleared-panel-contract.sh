#!/usr/bin/env bash
# Holds the cleared-panel contract on App/Assets.cpp's two full-panel draws.
#
# CardManager::drawCurrent() does not clear before a card's draw(), so a card
# that draws nothing leaves whatever the card before it put there. Both of these
# functions answer false without reaching the Display call that clears - one when
# the asset is not on the card, one when the buffer is empty - and the two picture
# cards draw nothing on a false return. That left the previous card on the glass
# for a whole dwell with the new card's clock and buttons over it.
#
# A text gate in the same spirit as check-ram-budget-call.sh beside it, which
# holds the same file's calls into RamPictureBudget.h: no host test compiles
# App/Assets.cpp, so the shape of these two returns is the only thing that can be
# checked here. What this cannot see is whether clearPanel() actually clears.
#
# README.md's "What the cleared-panel contract is for" is the design and
# TEST_PLAN.md section 39 is what this covers.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

echo "==> The cleared-panel contract on Assets.cpp's full-panel draws"

TARGET=App/Assets.cpp
FAILED=0

# Each entry: the function's opening line, and how many lines of it this gate
# reads. The early return is the first thing in each, ahead of any decode.
check_clears() {
  local opener="$1" window="$2" label="$3"
  local start
  start=$(grep -n -F -- "$opener" "$TARGET" | head -1 | cut -d: -f1 || true)
  if [ -z "$start" ]; then
    echo "ERROR: $TARGET no longer has '$opener' - this gate names what it holds." >&2
    FAILED=1
    return
  fi
  local body
  body=$(sed -n "${start},$((start + window))p" "$TARGET")
  if ! printf '%s' "$body" | grep -q "Display::clearPanel();"; then
    echo "ERROR: $label returns false without clearing the panel." >&2
    echo "       CardManager::drawCurrent() does not clear before a card's draw(), and" >&2
    echo "       both picture cards draw nothing when this answers false - so the card" >&2
    echo "       before them stays on the glass for the whole dwell with the new card's" >&2
    echo "       chrome over it. See README.md, \"What the cleared-panel contract is for\"." >&2
    FAILED=1
    return
  fi
  echo "    $label clears the panel before returning false"
}

check_clears "bool drawCached(const String& id) {" 20 "drawCached()'s not-on-the-card return"
check_clears "bool drawRam(const String& id, const RamAssetBuffer& buffer) {" 14 \
  "drawRam()'s empty-buffer return"

# The other half. A logo layered onto a card another draw already composed must
# not wipe it, so the in-rect variants deliberately do not clear.
for FN in "bool drawCachedInRect(" "bool drawRamInRect("; do
  START=$(grep -n -F -- "$FN" "$TARGET" | tail -1 | cut -d: -f1 || true)
  if [ -z "$START" ]; then
    echo "ERROR: $TARGET no longer has '$FN'." >&2
    FAILED=1
    continue
  fi
  if sed -n "${START},$((START + 14))p" "$TARGET" | grep -q "Display::clearPanel();"; then
    echo "ERROR: $FN clears the whole panel." >&2
    echo "       An in-rect draw is layered onto a card another call composed." >&2
    FAILED=1
  else
    echo "    $FN leaves the rest of the panel alone"
  fi
done

if [ "$FAILED" != "0" ]; then
  exit 1
fi

echo "PASS: cleared-panel contract - both full-panel draws clear before a false return, and neither in-rect draw does"
