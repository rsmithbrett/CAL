#!/usr/bin/env bash
# Holds the statements about a draw's content key that no compiler on a runner
# can reach: the order CardManager::noteShowing() resolves one in, that the
# resolved key reaches the impression, that CheckIn.cpp sends it under the name
# the server's rollup reads, and that registerCard() still checks the policy a
# registration states against the descriptor it was stated for.
#
# A draw's content key is what the server keys a quarter-hour period row on -
# CARD_IMPRESSIONS_DESIGN.md, "Which attribute says what the draw was about". A
# card that reports none collapses every item it draws into one row, so a report
# can say the card was up 512 times and cannot say which thing was on the glass.
#
# --- what this used to hold, and why it does not any more -------------------
#
# Until 2026-11-03 this script carried the per-card decision itself, as three
# lists of filenames - RESOLVES, POLICY_CONTENT and KEYLESS - and failed on a
# file in none of them. That was a text scan over C++ grading declarations it
# recognised, its patterns were widened twice after one of them stopped
# matching, and sportslive still reported nothing on 907 draws out of a file the
# lists called resolving.
#
# Cards::registerCard() now takes a Cards::KeyPolicy with no default, so a
# registration that names no policy does not compile, whatever the descriptor is
# called, however it was declared, and wherever its file sits. The call checks
# that policy against the descriptor's own listingId when the registration runs
# and refuses a disagreement. ci/test-key-policy.sh drives that rule on a host
# and proves the omitted argument does not build.
#
# So the lists are retired rather than kept beside the compiler's answer. Two
# records of one rule is how sportslive sat between a list and a document.
# BOOT_BUTTON_AND_IMPRESSIONS_DESIGN.md, "The decision is an argument", is the
# record.
#
# Which cards owe a key came back as its own gate on the same day, after that
# argument turned out to hold a decision stated rather than the right decision:
# ci/check-card-key-decisions.sh carries it per card id, not per filename, and
# compares it against the ids App/ defines and the policies its registrations
# state. Nothing about it is in this script, which is the point of its being
# somewhere else. BOOT_BUTTON_AND_IMPRESSIONS_DESIGN.md, "Which card owes which
# policy".
#
# --- what is left, and why it is still text ---------------------------------
#
# App/CardManager.cpp names a graphics library through Display.h's drawing path
# and does not compile on a host, so nothing in CI can call noteShowing() or
# registerCard(). The pins below are fixed-string matches against one statement
# apiece in that file and in App/CheckIn.cpp, in the same spirit as
# ci/check-score-guard.sh, ci/check-board-columns-call.sh and
# ci/check-ram-budget-call.sh beside it.
#
# They are about plumbing rather than about any card's decision: which fallback
# answers for the cards that resolve nothing, that the key reaches the wire, and
# that the registry's own check is wired in. None of them restates which cards
# owe a key.
#
# tests/personal-photos-key-test.cpp and
# tests/announcement-and-home-value-key-test.cpp drive three of those cards'
# resolvers through their own cases.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

FAIL=0
CHECKED=0

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

# One pin: a fixed string that has to be in the flattened file, and the message
# for its absence. Counted, so the figure printed at the bottom is what this run
# read rather than what it was written to read.
pin() {
  local path="$1" needle="$2" message="$3"
  CHECKED=$((CHECKED + 1))
  if [ ! -f "$path" ]; then
    say "$path" "not found, so this check read nothing."
    return
  fi
  if ! has "$(cat "$path")" "$needle"; then
    say "$path" "$message"
  fi
}

# --- the registry still checks what a registration states -------------------
#
# The compiler requires the argument. Whether the descriptor handed over agrees
# with it is known only once the registration runs, and this is the call that
# asks. Dropping it leaves every call site compiling and every card registering,
# including one that states Resolves and assigns nothing.
pin App/CardManager.cpp \
  'if(!keyPolicyHolds(spec,keyPolicy)){logKeyPolicyRefusal(spec,keyPolicy);returnfalse;}' \
  "registerCard() does not open with
       'if (!keyPolicyHolds(spec, keyPolicy)) { logKeyPolicyRefusal(spec, keyPolicy);
       return false; }'. The KeyPolicy argument is required at every call site by the
       compiler, and this is the only thing that reads it. Without it a card can state
       Resolves, assign no listingId, register anyway and report nothing on every draw."

# --- the fallback order the two policy cards rest on ------------------------
#
# Graphic and QrText report a key without a resolver, which only holds while
# noteShowing() falls back to the policy. Dropping either fallback takes the key
# off every graphic and every QR card in the fleet, and both modules keep
# compiling and keep drawing.
#
# The resolver call is pinned as well as the fallbacks: asking for the item's key
# after the fallbacks, or not at all, leaves every card that resolves its own key
# reporting whatever the policy happens to name.
pin App/CardManager.cpp \
  'StringcontentKey=card.listingId!=nullptr?card.listingId(gCurrent.item):String();' \
  "noteShowing() does not open with
       'String contentKey = card.listingId != nullptr ? card.listingId(gCurrent.item) :
       String();'. The cards that resolve their own key reach it through no other call, so
       a card's own answer has to be asked for first and asked for by item."

pin App/CardManager.cpp \
  'if(contentKey.length()==0){if(card.assetId[0]!='"'"'\0'"'"'){contentKey=card.assetId;}elseif(card.qrData[0]!='"'"'\0'"'"'){contentKey=card.qrData;}}' \
  "noteShowing()'s fallbacks are not
       'if (contentKey.length() == 0) { if (card.assetId[0] != 0) { contentKey =
       card.assetId; } else if (card.qrData[0] != 0) { contentKey = card.qrData; } }'.
       Graphic and QrText report no key of their own, so dropping or reordering these takes
       the key off every graphic and every QR card in the fleet while both keep drawing."

# The key reaches the buffer. An assignment that computes contentKey and hands
# beginShowing() something else leaves every check above satisfied.
pin App/CardManager.cpp \
  'Impressions::beginShowing(card.id,contentKey.c_str(),summary.c_str(),arrival);' \
  "noteShowing() does not hand the resolved key to
       'Impressions::beginShowing(card.id, contentKey.c_str(), summary.c_str(), arrival)'.
       A key computed and not passed is a key nothing reports."

# --- the wire carries it ----------------------------------------------------
#
# CheckIn.cpp omits the attributes object when both the key and the summary are
# empty, which is what makes a keyless draw store `{}`. The pin is that a key
# that IS present goes out under the name the rollup reads.
pin App/CheckIn.cpp 'attributes["contentKey"]=drawn.contentKey;' \
  "the draw's key does not go out as 'attributes[\"contentKey\"] = drawn.contentKey;'.
       CardImpressionRollup.ContentKeyNames reads compassPid, mlsNumber, contentKey and
       assetId in that order, and every card in this fleet sends its key under contentKey;
       renaming it here stores the value where no subject looks."

# --- how much it read -------------------------------------------------------
#
# The figure printed is one this run measured. A gate whose patterns stopped
# matching passes by finding nothing, and this count is the only way to see that
# from the outside, so it prints whether or not anything failed.
echo "    $CHECKED statements pinned across App/CardManager.cpp and App/CheckIn.cpp"

if [ "$CHECKED" -lt 5 ]; then
  echo "ERROR: only $CHECKED statements were pinned and there were 5 on 2026-11-03. A pin" >&2
  echo "       deleted from this script takes its statement with it, and the per-card" >&2
  echo "       decision this script used to carry is the compiler's now - see" >&2
  echo "       ci/test-key-policy.sh - so nothing else is left holding these five." >&2
  FAIL=1
fi

if [ "$FAIL" -ne 0 ]; then
  echo "       See CARD_IMPRESSIONS_DESIGN.md, 'Which attribute says what the draw was" >&2
  echo "       about', BOOT_BUTTON_AND_IMPRESSIONS_DESIGN.md, 'The decision is an" >&2
  echo "       argument', and the remarks at the top of this script." >&2
  exit 1
fi

echo "==> the resolver is asked first, the key reaches the wire, and the registry checks what a registration states"
