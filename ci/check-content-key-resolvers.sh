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
# The grading is per card descriptor, not per file. A file can register several:
# Sports.cpp has a helper three team cards share and a descriptor built by hand
# for the live one. Reading the file as a whole passed the hand-built keyless
# descriptor on the strength of its keyed neighbour, so sportslive reported
# nothing on 907 draws out of a file this gate called resolving.
#
# Every Cards::CardSpec a file declares is graded, every one has to be handed to
# Cards::registerCard() by its own name, and a resolver assigned through any
# other lvalue fails. `Cards::CardSpec& s = spec; s.listingId = &f;` satisfies a
# search for `spec.listingId=` while telling nobody which card got one.
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
# Cards that resolve their own key, because the module is the only thing that can
# name what it drew. Most of these walk a feed and only the module knows which
# item is up. Announcement.cpp and HomeValue.cpp are here for a narrower reason:
# an announcement's words arrive on the card policy and a valuation's address
# arrives on the check-in response, and neither lands in assetId or qrData, which
# are the only two fields noteShowing() falls back to.
RESOLVES="Aircraft.cpp Announcement.cpp FeaturedListings.cpp HomeValue.cpp Listings.cpp
          PersonalPhotos.cpp Sports.cpp"

# Cards whose content arrives on the card policy, so noteShowing()'s assetId and
# qrData fallbacks answer for them and a resolver would repeat what is already in
# front of it. Graphic draws the asset the policy names; QrText encodes the
# payload the policy carries.
POLICY_CONTENT="Graphic.cpp QrText.cpp"

# Cards with nothing to name. A clock, a moon phase, a tide table and a forecast
# are about a place and a time rather than a thing, and an empty key is the state
# the period row has for them.
#
# HomeValue.cpp and Announcement.cpp were here until 2026-11-03, reporting a
# summary on every draw and no key. The owner's call that day is that both owe one,
# so they sit in RESOLVES above; CARD_IMPRESSIONS_DESIGN.md, "What a home-value and
# an announcement draw report", is the record.
KEYLESS="Calendar.cpp ClockDate.cpp Forecast.cpp
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

# The names a file declares as a Cards::CardSpec of its own, one per card it
# registers.
#
# Whitespace after the type is required, which is what keeps a reference or a
# pointer declaration out: `Cards::CardSpec&` and `Cards::CardSpec*` both put a
# character where that whitespace has to be. Those are not cards, they are second
# names for one, and the assignment tally below is what refuses either as a route
# to a resolver.
specs_in() {
  sed 's://.*::' "App/$1" \
    | grep -oE 'Cards::CardSpec[[:space:]]+[A-Za-z_][A-Za-z0-9_]*' \
    | awk '{ print $NF }' | sort -u || true
}

# How many files were graded and how many descriptors in them, printed whether or
# not anything failed. A gate whose pattern stopped matching passes by finding
# nothing, and the only way to see that from the outside is to have it say how
# much it looked at. Both figures matter: the file count alone stayed at 16 while
# a keyless card hid inside one of them.
REGISTRAR_COUNT=$(printf '%s\n' $REGISTRARS | wc -l | tr -d ' ')
DESCRIPTOR_COUNT=0
for file in $REGISTRARS; do
  n=$(specs_in "$file" | wc -l | tr -d ' ')
  DESCRIPTOR_COUNT=$((DESCRIPTOR_COUNT + n))
done

echo "    $REGISTRAR_COUNT files in App/ register a card, between them $DESCRIPTOR_COUNT card descriptors"
if [ "$REGISTRAR_COUNT" -lt 16 ]; then
  echo "ERROR: only $REGISTRAR_COUNT registering files were found and there were 16 on
       2026-11-03. Either cards were removed, or the grep above stopped matching and
       this gate is checking less than it reads." >&2
  FAIL=1
fi
if [ "$DESCRIPTOR_COUNT" -lt 17 ]; then
  echo "ERROR: only $DESCRIPTOR_COUNT card descriptors were found and there were 17 on
       2026-11-03. Either cards were removed, or specs_in() stopped matching and every
       grade below was handed out on a shorter list than the files hold." >&2
  FAIL=1
fi

for file in $REGISTRARS; do
  CODE=$(sed 's://.*::' "App/$file")
  FLAT=$(printf '%s\n' "$CODE" | tr -d ' \t\r\n')
  SPECS=$(specs_in "$file")

  if [ -z "$SPECS" ]; then
    say "App/$file" "calls Cards::registerCard and declares no Cards::CardSpec this gate
         can name, so none of its cards were graded. Declare the descriptor as
         'Cards::CardSpec <name>;' and fill it in, the way every other card module here
         does."
    continue
  fi

  if ! listed "$file"; then
    say "App/$file" "registers a card and is in none of this gate's three lists.
         Decide what its draws report: a listingId() resolver (RESOLVES), the policy's
         assetId or qrData (POLICY_CONTENT), or no key at all (KEYLESS), then add it.
         A card reporting no key collapses every item it draws into one period row."
    continue
  fi

  # Every listingId assignment in the file lands on a descriptor named above.
  # The count is the pin rather than the spelling: an assignment through a
  # reference, a pointer or a dereference reads as a resolver to the compiler and
  # names no card here, so the two tallies part company and this fails.
  ASSIGNED_ANYWHERE=$(printf '%s' "$FLAT" | grep -oF 'listingId=' | wc -l | tr -d ' ' || true)
  ASSIGNED_TO_SPECS=0
  for spec in $SPECS; do
    n=$(printf '%s' "$FLAT" | grep -oF "$spec.listingId=" | wc -l | tr -d ' ' || true)
    ASSIGNED_TO_SPECS=$((ASSIGNED_TO_SPECS + n))
  done
  if [ "$ASSIGNED_ANYWHERE" != "$ASSIGNED_TO_SPECS" ]; then
    say "App/$file" "has $ASSIGNED_ANYWHERE listingId assignment(s) and only
         $ASSIGNED_TO_SPECS of them land on a Cards::CardSpec this gate can name
         ($(echo $SPECS)). A resolver assigned through a reference, a pointer or a
         dereference reaches the card and tells nobody which card got one. Assign it on
         the descriptor by its own name."
  fi

  for spec in $SPECS; do
    if ! printf '%s' "$FLAT" | grep -qF "Cards::registerCard($spec)"; then
      say "App/$file" "declares the card descriptor '$spec' and never hands it to
           Cards::registerCard($spec). A descriptor registered through another name is
           graded by nothing here."
      continue
    fi

    # Assigned, and not assigned nullptr. The seven that resolve spell the
    # right-hand side four different ways - `&cardListingId`, `cardListingId` and
    # `listingIdFor(index)` among them - so the pin is the assignment rather than
    # what it names, and `= nullptr` is the one value that assigns no resolver.
    RESOLVER=no
    if printf '%s' "$FLAT" | grep -qF "$spec.listingId=" \
       && ! printf '%s' "$FLAT" | grep -qF "$spec.listingId=nullptr"; then
      RESOLVER=yes
    fi

    if in_list "$file" "$RESOLVES" && [ "$RESOLVER" = no ]; then
      say "App/$file" "is listed as resolving its own content key and its descriptor
           '$spec' assigns no listingId. That card's draws report nothing, and the period
           rows for every item it draws collapse into one. A file whose cards genuinely
           differ needs its lists here split per card id rather than per file."
    fi

    if in_list "$file" "$POLICY_CONTENT" && [ "$RESOLVER" = yes ]; then
      say "App/$file" "is listed as taking its key off the card policy and its descriptor
           '$spec' now assigns listingId. Move it to RESOLVES, because the list is what is
           wrong."
    fi

    if in_list "$file" "$KEYLESS" && [ "$RESOLVER" = yes ]; then
      say "App/$file" "is listed as reporting no content key and its descriptor '$spec' now
           assigns listingId. Move it to RESOLVES, because the list is what is wrong."
    fi
  done
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
