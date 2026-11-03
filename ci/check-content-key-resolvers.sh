#!/usr/bin/env bash
# Holds the decision every card module has to make about what its draws report.
#
# A draw's content key is what the server keys a quarter-hour period row on -
# CARD_IMPRESSIONS_DESIGN.md, "Which attribute says what the draw was about". A
# card that reports none collapses every item it draws into one row, so a report
# can say the card was up 512 times and cannot say which thing was on the glass.
#
# CardManager::noteShowing() resolves the key three ways, in order: the card's
# own listingId(), then the policy entry's assetId, then its qrData. A module
# that offers no resolver and draws content the policy does not name reports
# nothing, and nothing anywhere fails. App/PersonalPhotos.cpp did exactly that,
# and 1,022 draws across five displays stored an empty attributes document.
#
# So every file registering a card is in one of three lists below and the gate
# fails on a file in none of them. A new card is a new file calling
# Cards::registerCard(), which lands in no list, which is the point: the decision
# gets made rather than defaulted.
#
# The lists are also checked the other way. A file listed as keyless that has
# since gained a resolver fails too, because then the list is what is wrong, and
# a list nobody has to maintain stops describing anything.
#
# A text gate rather than a test, in the same spirit as ci/check-score-guard.sh
# and ci/check-board-columns-call.sh beside it. CardManager.cpp names a graphics
# library and does not compile on a host, so the fallback order it applies can
# only be held this way. tests/personal-photos-key-test.cpp is what drives the
# photograph card's resolver through its own cases.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

FAIL=0

say() {
  echo "ERROR: $1: $2" >&2
  FAIL=1
}

# A file read as one run of characters with the line comments, then the spacing
# and the line breaks, taken out, so a statement that wraps reads the same as one
# that fits.
#
# Line comments go first because every pattern here is a fixed string and a fixed
# string cannot tell a statement from a commented-out copy of one. These files
# carry remarks that quote code, so without this a registration deleted and left
# behind as `// spec.listingId = &cardListingId;` would satisfy its own check.
#
# \r as well as space, tab and newline: these files are checked out CRLF under
# core.autocrlf, and a carriage return in the middle of a joined statement stops
# every pattern below from matching, which would make this gate pass by finding
# nothing rather than by checking anything.
flatten() {
  sed 's://.*::' | tr -d ' \t\r\n'
}

has() {
  printf '%s\n' "$1" | flatten | grep -qF "$2"
}

lacks() {
  ! has "$1" "$2"
}

# --- the three lists ---------------------------------------------------------
#
# Cards that resolve their own key, because what they draw comes off a feed and
# only the module knows which item is up.
RESOLVES="Aircraft.cpp FeaturedListings.cpp Listings.cpp PersonalPhotos.cpp Sports.cpp"

# Cards whose content arrives on the card policy, so noteShowing()'s assetId and
# qrData fallbacks answer for them and a resolver would repeat what is already in
# front of it. Graphic draws the asset the policy names; QrText encodes the
# payload the policy carries.
POLICY_CONTENT="Graphic.cpp QrText.cpp"

# Cards with nothing to name. A clock, a moon phase, a tide table and a forecast
# are about a place and a time rather than a thing, and an empty key is the state
# the period row has for them.
#
# HomeValue.cpp and Announcement.cpp are here and they are not the same case as
# the rest: both report a summary on every draw and no key, so their period rows
# carry a label and group as one. Whether either owes a key is an open question
# for the owner, recorded in CARD_IMPRESSIONS_DESIGN.md rather than decided here.
KEYLESS="Announcement.cpp Calendar.cpp ClockDate.cpp Forecast.cpp HomeValue.cpp
         IssFlyover.cpp MoonPhase.cpp SunMoon.cpp Tides.cpp"

listed() {
  for name in $RESOLVES $POLICY_CONTENT $KEYLESS; do
    [ "$name" = "$1" ] && return 0
  done
  return 1
}

in_list() {
  for name in $2; do
    [ "$name" = "$1" ] && return 0
  done
  return 1
}

# --- every file that registers a card ---------------------------------------
REGISTRARS=$(grep -l 'Cards::registerCard' App/*.cpp 2>/dev/null | xargs -n1 basename | sort)

if [ -z "$REGISTRARS" ]; then
  echo "ERROR: no file in App/ calls Cards::registerCard, so this gate checked nothing." >&2
  exit 1
fi

# How many files were graded, printed whether or not anything failed. A gate
# whose pattern stopped matching passes by finding nothing, and the only way to
# see that from the outside is to have it say how much it looked at.
REGISTRAR_COUNT=$(printf '%s\n' $REGISTRARS | wc -l | tr -d ' ')
echo "    $REGISTRAR_COUNT files in App/ register a card"
if [ "$REGISTRAR_COUNT" -lt 16 ]; then
  echo "ERROR: only $REGISTRAR_COUNT registering files were found and there were 16 on
       2026-11-03. Either cards were removed, or the grep above stopped matching and
       this gate is checking less than it reads." >&2
  FAIL=1
fi

for file in $REGISTRARS; do
  TEXT=$(cat "App/$file")
  # Assigned, and not assigned nullptr. The five that resolve spell the
  # right-hand side four different ways - `&cardListingId`, `cardListingId` and
  # `listingIdFor(index)` among them - so the pin is the assignment rather than
  # what it names, and `= nullptr` is the one value that assigns no resolver.
  RESOLVER=no
  if has "$TEXT" 'spec.listingId=' && lacks "$TEXT" 'spec.listingId=nullptr'; then
    RESOLVER=yes
  fi

  if ! listed "$file"; then
    say "App/$file" "registers a card and is in none of this gate's three lists.
         Decide what its draws report: a listingId() resolver (RESOLVES), the policy's
         assetId or qrData (POLICY_CONTENT), or no key at all (KEYLESS), then add it.
         A card reporting no key collapses every item it draws into one period row."
    continue
  fi

  if in_list "$file" "$RESOLVES" && [ "$RESOLVER" = no ]; then
    say "App/$file" "is listed as resolving its own content key and assigns no
         spec.listingId. Its draws report nothing, and the period rows for every item it
         draws collapse into one."
  fi

  if in_list "$file" "$POLICY_CONTENT" && [ "$RESOLVER" = yes ]; then
    say "App/$file" "is listed as taking its key off the card policy and now assigns
         spec.listingId. Move it to RESOLVES, because the list is what is wrong."
  fi

  if in_list "$file" "$KEYLESS" && [ "$RESOLVER" = yes ]; then
    say "App/$file" "is listed as reporting no content key and now assigns
         spec.listingId. Move it to RESOLVES, because the list is what is wrong."
  fi
done

# --- and the fallback order the two policy cards rest on ---------------------
#
# Graphic and QrText report a key without a resolver, which only holds while
# noteShowing() falls back to the policy. Dropping either fallback takes the key
# off every graphic and every QR card in the fleet, and both modules keep
# compiling and keep drawing.
#
# The resolver call is in the pin as well as the fallbacks: asking for the item's
# key after the fallbacks, or not at all, leaves the five cards above reporting
# whatever the policy happens to name.
HOOK=App/CardManager.cpp
if [ ! -f "$HOOK" ]; then
  say "$HOOK" "not found, so the fallback order below checked nothing."
else
  HOOK_TEXT=$(cat "$HOOK")

  if lacks "$HOOK_TEXT" 'StringcontentKey=card.listingId!=nullptr?card.listingId(gCurrent.item):String();'; then
    say "$HOOK" "noteShowing() does not open with
         'String contentKey = card.listingId != nullptr ? card.listingId(gCurrent.item) :
         String();'. The five cards that resolve their own key reach it through no other
         call, so a card's own answer has to be asked for first and asked for by item."
  fi

  if lacks "$HOOK_TEXT" 'if(contentKey.length()==0){if(card.assetId[0]!='"'"'\0'"'"'){contentKey=card.assetId;}elseif(card.qrData[0]!='"'"'\0'"'"'){contentKey=card.qrData;}}'; then
    say "$HOOK" "noteShowing()'s fallbacks are not
         'if (contentKey.length() == 0) { if (card.assetId[0] != 0) { contentKey =
         card.assetId; } else if (card.qrData[0] != 0) { contentKey = card.qrData; } }'.
         Graphic and QrText report no key of their own, so dropping or reordering these
         takes the key off every graphic and every QR card in the fleet while both keep
         drawing."
  fi

  # The key reaches the buffer. An assignment that computes contentKey and hands
  # beginShowing() something else leaves every check above satisfied.
  if lacks "$HOOK_TEXT" 'Impressions::beginShowing(card.id,contentKey.c_str(),summary.c_str(),arrival);'; then
    say "$HOOK" "noteShowing() does not hand the resolved key to
         'Impressions::beginShowing(card.id, contentKey.c_str(), summary.c_str(),
         arrival)'. A key computed and not passed is a key nothing reports."
  fi
fi

# --- the wire carries it -----------------------------------------------------
#
# CheckIn.cpp omits the attributes object when both the key and the summary are
# empty, which is what makes a keyless draw store `{}`. The pin is that a key
# that IS present goes out under the name the rollup reads.
WIRE=App/CheckIn.cpp
if [ ! -f "$WIRE" ]; then
  say "$WIRE" "not found, so the attribute name below checked nothing."
elif lacks "$(cat "$WIRE")" 'attributes["contentKey"]=drawn.contentKey;'; then
  say "$WIRE" "the draw's key does not go out as 'attributes[\"contentKey\"] =
       drawn.contentKey;'. CardImpressionRollup.ContentKeyNames reads compassPid,
       mlsNumber, contentKey and assetId in that order, and every card in this fleet
       sends its key under contentKey; renaming it here stores the value where no
       subject looks."
fi

if [ "$FAIL" -ne 0 ]; then
  echo "       See CARD_IMPRESSIONS_DESIGN.md, 'Which attribute says what the draw was" >&2
  echo "       about', and the remarks at the top of ci/check-content-key-resolvers.sh." >&2
  exit 1
fi

echo "==> every card registering here has decided what its draws report, and the key reaches the wire"
