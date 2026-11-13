#!/usr/bin/env bash
# Holds the owner's rule that no picture means no card: a card cannot put a
# message on the glass in place of its own content.
#
# A display was photographed showing "No picture to show / This photograph could
# not be loaded." Four cards could do that, and each now reports zero items and
# is passed over instead. Three of the five message sites were only reachable by
# a caller that did not ask first, so no host case can get to them, and the
# fourth and fifth are a draw path a test can reach only through the card's own
# stubs. A text gate is what covers all five at once.
#
# What this cannot see: whether a card draws the right thing, or whether the
# scheduler passes it over. README.md's "No picture means no card" section is
# the design and TEST_PLAN.md section 38 is what the host cases cover.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

echo "==> No content means no card, not a screen saying so"

# The four card files, and the one place the screen is still right.
QUIET=(App/PersonalPhotos.cpp App/Graphic.cpp App/Announcement.cpp App/QrText.cpp)
FAILED=0

for FILE in "${QUIET[@]}"; do
  if [ ! -f "$FILE" ]; then
    echo "ERROR: $FILE is missing - this gate names the files it holds." >&2
    exit 1
  fi
  HITS=$(grep -c "showNoContent" "$FILE" || true)
  # Any mention at all, comments included: a card file naming this function is
  # either calling it or arguing for calling it.
  if [ "$HITS" != "0" ]; then
    echo "ERROR: $FILE calls Display::showNoContent() $HITS time(s)." >&2
    echo "       A card with nothing to show reports zero items and is passed over." >&2
    echo "       It does not put a screen up explaining the absence - see README.md," >&2
    echo "       \"No picture means no card\"." >&2
    FAILED=1
  else
    echo "    $FILE draws its content or nothing"
  fi
done

# The other half of the rule. These two screens are about the whole rotation
# having nothing rather than about one card, and a panel with nothing at all to
# draw has to say something - so a change that quietens them too has gone
# further than the rule asks.
# Calls, not mentions: one line above the first of them explains why the budget
# is reset there and names the function in a comment.
KEPT=$(grep -c "Display::showNoContent(" App/CardManager.cpp || true)
if [ "$KEPT" != "2" ]; then
  echo "ERROR: App/CardManager.cpp calls Display::showNoContent() $KEPT time(s), expected 2." >&2
  echo "       The rotation-empty screen and the no-cards-registered screen both stay:" >&2
  echo "       a panel with nothing at all to draw has to say something." >&2
  FAILED=1
else
  echo "    App/CardManager.cpp keeps its $KEPT rotation-wide screens"
fi

if [ "$FAILED" != "0" ]; then
  exit 1
fi

echo "PASS: no content means no card - the four cards draw content or nothing, and the two rotation-wide screens stay"
