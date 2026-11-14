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
# App/Calendar.cpp is deliberately not on the list. It has the same shape and
# its items expire on a clock rather than on a fetch, so its draw carries a
# fallback for exactly that race - but what that fallback does is draw another
# real event, or nothing, and never a message. It is already what this rule
# asks for. It also contains the token this gate greps for, inside a comment
# recording why that approach was rejected there, so listing it would fail the
# gate on the documentation of its own compliance. Teaching a grep to tell a
# call from a comment makes it a parser, and a parser is a thing that can be
# wrong quietly. See README.md's "No picture means no card" for the reasoning
# written out.
#
# What this cannot see: whether a card draws the right thing, or whether the
# scheduler passes it over. README.md's "No picture means no card" section is
# the design and TEST_PLAN.md sections 38 and 40 are what the host cases cover.
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
  # Any mention at all, comments included: none of these four names the function
  # today, so a mention is a call or an argument for one.
  if [ "$HITS" != "0" ]; then
    echo "ERROR: $FILE calls Display::showNoContent() $HITS time(s)." >&2
    echo "       A card with nothing to show reports zero items and is passed over." >&2
    echo "       It does not put a screen up explaining the absence - see README.md," >&2
    echo "       \"No picture means no card\"." >&2
    FAILED=1
  fi

  # The rule is about putting a message on the glass where a card's content
  # goes, not about one function's name. Display has four other screens that do
  # the same thing in the same place, and a card re-introducing its message
  # through any of them reads identically to a household.
  for SCREEN in showStatus showFailure showForecastStatus showListingsStatus; do
    SCREEN_HITS=$(grep -c "Display::$SCREEN" "$FILE" || true)
    if [ "$SCREEN_HITS" != "0" ]; then
      echo "ERROR: $FILE calls Display::$SCREEN() $SCREEN_HITS time(s)." >&2
      echo "       That screen is a message where the card's content goes, which is the" >&2
      echo "       thing the owner photographed. A card with nothing to show reports zero" >&2
      echo "       items and is passed over - see README.md, \"No picture means no card\"." >&2
      FAILED=1
    fi
  done

  if [ "$HITS" = "0" ]; then
    echo "    $FILE draws its content or nothing"
  fi
done

# The two the owner added after the first four. Both lived inside Display.cpp
# rather than in a card, drawn where that card's content goes, so neither the
# loop above nor any host test could see them. Held by the words themselves.
for GONE in "No games today" "Cannot display code"; do
  if grep -qF -- "$GONE" App/Display.cpp; then
    echo "ERROR: App/Display.cpp draws \"$GONE\" where a card's content goes." >&2
    echo "       The sports board with no games and the QR card with a payload that" >&2
    echo "       will not encode both take no turn instead - see README.md." >&2
    FAILED=1
  else
    echo "    App/Display.cpp no longer draws \"$GONE\""
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
  echo "ERROR: App/CardManager.cpp calls Display::showNoContent() $KEPT time(s)." >&2
  echo "       Both of its two calls have to stay." >&2
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
