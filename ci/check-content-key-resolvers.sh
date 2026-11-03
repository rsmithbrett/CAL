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
# --- what the census is anchored on ------------------------------------------
#
# The registration calls are the census and the descriptor declarations are only
# how a registered card gets graded. That order matters, because the two can
# disagree in exactly one direction: a descriptor declared in a spelling the
# patterns below do not recognise still has to be handed to
# Cards::registerCard() by name to become a card at all. So REGISTRATIONS is
# counted straight off `Cards::registerCard(` and every argument it names has to
# be a descriptor this gate recognised. A spelling nobody here thought of raises
# the registration tally without raising the descriptor tally, the two part
# company, and the file fails naming the argument it cannot place. A declaration
# pattern that stops matching therefore fails loudly rather than grading a
# shorter list.
#
# Four spellings are recognised as a declaration: `Cards::CardSpec name;`
# including when the type and the name sit on separate lines, `auto name =
# Cards::CardSpec{}`, and either alias form (`using A = Cards::CardSpec;` or
# `typedef Cards::CardSpec A;`) followed by `A name;`. A declarator list
# (`Cards::CardSpec a, b;`) names only the first, and that is deliberate: the
# second name then reaches the registration tally and the file fails on the
# imbalance rather than on a parse of C++ declarators this script is in no
# position to write.
#
# Whitespace is squeezed to one space before any of that runs, so a statement
# that wraps across lines reads the same as one that fits, and the whitespace the
# type needs after it is still there to keep `Cards::CardSpec&` and
# `Cards::CardSpec*` out. Those are not cards, they are second names for one, and
# the assignment tally below is what refuses either as a route to a resolver.
#
# `registerCard(` is also accounted for in full across every file swept. Every
# occurrence is either the declaration in Cards.h, the definition in
# CardManager.cpp, or a qualified `Cards::registerCard(` call, and anything else
# fails. That is what catches a call reaching the registry under another name,
# through a `using` declaration or a function pointer, which no list here would
# ever see.
#
# --- how much it read --------------------------------------------------------
#
# Every figure printed is one this run measured, and the floors are set from the
# measurement rather than asserted ahead of it. A gate whose pattern stopped
# matching passes by finding nothing, and the figures are the only way to see
# that from the outside, so they print whether or not anything failed. The file
# sweep's own breadth is in there for the same reason: the search was one level
# deep and read no headers once, and a card registered from either place was
# graded by nothing while every other figure stayed where it was.
#
# The listingId assignment total prints without a floor. It moves whenever a card
# legitimately changes lists, and a resolver that goes missing is already a named
# failure from the per-descriptor grading below.
#
# --- what this cannot hold ---------------------------------------------------
#
# This is a text gate, and the widening above is the second time its patterns
# have been widened after one of them stopped matching. What would hold it
# outright is the registry itself: registerCard() is the one function every card
# reaches, it has every descriptor in its hand, and a decision passed to it as an
# argument would be checked by the compiler at every call site no matter how the
# descriptor was spelled or which file it sat in. CardManager.cpp names a
# graphics library and does not compile on a host, so nothing in CI can walk that
# registry today. Until something can, the figures above are the whole defence,
# and they are a statement about what was read, not a proof that a card cannot
# hide.
#
# A text gate in the same spirit as ci/check-score-guard.sh and
# ci/check-board-columns-call.sh beside it. The fallback order noteShowing()
# applies can only be held this way for the same compile reason.
# tests/personal-photos-key-test.cpp is what drives the photograph card's
# resolver through its own cases.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

FAIL=0

say() {
  echo "ERROR: $1: $2" >&2
  FAIL=1
}

# A file read as one run of characters with the line comments, then the spacing
# and the line breaks, taken out, so a statement that wraps reads the same as one
# that fits. Used by the noteShowing() and CheckIn.cpp checks at the bottom,
# which are fixed-string matches against one statement apiece.
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
# Keyed on the path under App/, so a file in a subdirectory is named by that path
# and lands in no list until somebody puts it in one.
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

# --- every source file under App/ -------------------------------------------
#
# Recursive, and headers as well as translation units, because a registration
# reaches the registry from wherever it is written. App/build holds generated
# copies of these same files and is pruned: it is gitignored output, and reading
# it would grade a stale duplicate of every card.
mapfile -t SOURCES < <(find App -type d -name build -prune -o -type f \
  \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' -o -name '*.c' -o -name '*.ino' \) \
  -print | sort)

SOURCE_COUNT=${#SOURCES[@]}

if [ "$SOURCE_COUNT" -eq 0 ]; then
  echo "ERROR: no source file was found under App/, so this gate read nothing." >&2
  exit 1
fi

# One pass over every file, reporting what each holds as tab-separated records.
#
# Comments come out first, block as well as line, as a scan with the state
# carried across records: these files explain their own registration in remarks
# that quote it, and a fixed-string search cannot tell a statement from a
# commented copy of one. \r goes with them, because a carriage return inside a
# joined statement stops every pattern here from matching.
#
# Then two readings of the same file. SQUEEZED keeps one space wherever there was
# any whitespace, which is what the declaration patterns need to separate a type
# from a name. TIGHT has the spaces gone as well, which is what the registration
# and assignment counts read, so `spec.listingId = &f` and `spec.listingId=&f`
# count the same.
RECORDS=$(awk '
  function countOf(hay, needle,    n, p, start) {
    n = 0
    start = 1
    while (1) {
      p = index(substr(hay, start), needle)
      if (p == 0) { return n }
      n++
      start = start + p - 1 + length(needle)
    }
  }

  function report(path,    rel, tight, i, rest, piece, name, k, total, strays,
                  regs, tok, pos, q, a, cnt, nulls) {
    rel = path
    sub(/^App\//, "", rel)

    # Runs of whitespace down to one space, which is what lets a declaration
    # wrapped across lines read like one that fitted, and what leaves the space
    # the type needs after it for the patterns below.
    gsub(/[ \t]+/, " ", SQUEEZED)

    tight = SQUEEZED
    gsub(/ /, "", tight)

    split("", decl)
    split("", alias)

    # `Cards::CardSpec name`, with the whitespace after the type keeping a
    # reference or a pointer declaration out.
    rest = SQUEEZED
    while (match(rest, /Cards::CardSpec [A-Za-z_][A-Za-z0-9_]*/)) {
      piece = substr(rest, RSTART, RLENGTH)
      decl[substr(piece, length("Cards::CardSpec ") + 1)] = 1
      rest = substr(rest, RSTART + RLENGTH)
    }

    # `auto name = Cards::CardSpec{}`, where the brace sits where the pattern
    # above wants its whitespace.
    rest = SQUEEZED
    while (match(rest, /auto [A-Za-z_][A-Za-z0-9_]* ?= ?Cards::CardSpec/)) {
      piece = substr(rest, RSTART, RLENGTH)
      sub(/^auto /, "", piece)
      sub(/ ?= ?Cards::CardSpec$/, "", piece)
      decl[piece] = 1
      rest = substr(rest, RSTART + RLENGTH)
    }

    # Either alias form, and then the names declared through the alias.
    rest = SQUEEZED
    while (match(rest, /using [A-Za-z_][A-Za-z0-9_]* ?= ?Cards::CardSpec ?;/)) {
      piece = substr(rest, RSTART, RLENGTH)
      sub(/^using /, "", piece)
      sub(/ ?= ?Cards::CardSpec ?;$/, "", piece)
      alias[piece] = 1
      rest = substr(rest, RSTART + RLENGTH)
    }
    rest = SQUEEZED
    while (match(rest, /typedef Cards::CardSpec [A-Za-z_][A-Za-z0-9_]* ?;/)) {
      piece = substr(rest, RSTART, RLENGTH)
      sub(/^typedef Cards::CardSpec /, "", piece)
      sub(/ ?;$/, "", piece)
      alias[piece] = 1
      rest = substr(rest, RSTART + RLENGTH)
    }
    for (a in alias) {
      rest = SQUEEZED
      while (match(rest, "(^|[^A-Za-z0-9_:])" a " [A-Za-z_][A-Za-z0-9_]*")) {
        piece = substr(rest, RSTART, RLENGTH)
        k = index(piece, a " ")
        decl[substr(piece, k + length(a) + 1)] = 1
        rest = substr(rest, RSTART + RLENGTH)
      }
    }

    # Every registerCard( in the file, and the two forms that are the
    # declaration and the definition rather than a call. What is left over is a
    # call reaching the registry under some other name.
    total = countOf(tight, "registerCard(")
    regs = countOf(tight, "Cards::registerCard(")
    strays = total - regs - countOf(tight, "registerCard(constCardSpec&")

    printf "FILE\t%s\t%d\t%d\t%d\n", rel, regs, \
      countOf(tight, "listingId="), strays

    for (name in decl) { printf "DECL\t%s\t%s\n", rel, name }

    # What each registration names. A token that is not a bare identifier is
    # reported as it reads, so the message can quote it.
    pos = 1
    while (1) {
      q = index(substr(tight, pos), "Cards::registerCard(")
      if (q == 0) { break }
      pos = pos + q - 1 + length("Cards::registerCard(")
      rest = substr(tight, pos)
      k = index(rest, ")")
      if (k == 0) { tok = rest } else { tok = substr(rest, 1, k - 1) }
      printf "ARG\t%s\t%s\n", rel, tok
    }

    for (name in decl) {
      cnt = countOf(tight, name ".listingId=")
      nulls = countOf(tight, name ".listingId=nullptr")
      printf "ASSIGN\t%s\t%s\t%d\t%d\n", rel, name, cnt, nulls
    }
  }

  FILENAME != current {
    if (current != "") { report(current) }
    current = FILENAME
    SQUEEZED = ""
    inblock = 0
  }

  {
    line = $0
    sub(/\r$/, "", line)

    code = ""
    i = 1
    len = length(line)
    while (i <= len) {
      two = substr(line, i, 2)
      if (inblock) {
        if (two == "*/") { inblock = 0; i += 2 } else { i++ }
        continue
      }
      if (two == "/*") { inblock = 1; i += 2; continue }
      if (two == "//") { break }
      code = code substr(line, i, 1)
      i++
    }

    SQUEEZED = SQUEEZED " " code
  }

  END {
    if (current != "") { report(current) }
  }
' "${SOURCES[@]}")

declare -A FILE_REGS=() FILE_ASSIGNED=() FILE_STRAYS=()
declare -A SPECS_OF=() ARGS_OF=() SPEC_ASSIGNED=() SPEC_NULLED=()

REGISTRARS=""
REGISTRATION_COUNT=0
DESCRIPTOR_COUNT=0
ASSIGNMENT_COUNT=0
STRAY_COUNT=0

while IFS=$'\t' read -r tag file a b c; do
  case "$tag" in
    FILE)
      FILE_REGS["$file"]=$a
      FILE_ASSIGNED["$file"]=$b
      FILE_STRAYS["$file"]=$c
      STRAY_COUNT=$((STRAY_COUNT + c))
      if [ "$a" -gt 0 ]; then
        REGISTRARS="$REGISTRARS $file"
        REGISTRATION_COUNT=$((REGISTRATION_COUNT + a))
        ASSIGNMENT_COUNT=$((ASSIGNMENT_COUNT + b))
      fi
      ;;
    DECL)
      SPECS_OF["$file"]="${SPECS_OF[$file]-} $a"
      ;;
    ARG)
      ARGS_OF["$file"]="${ARGS_OF[$file]-} $a"
      ;;
    ASSIGN)
      SPEC_ASSIGNED["$file/$a"]=$b
      SPEC_NULLED["$file/$a"]=$c
      ;;
  esac
done <<< "$RECORDS"

REGISTRARS=$(printf '%s\n' $REGISTRARS | sort)
REGISTRAR_COUNT=$(printf '%s\n' $REGISTRARS | grep -c . || true)

for file in $REGISTRARS; do
  n=$(printf '%s\n' ${SPECS_OF[$file]-} | grep -c . || true)
  DESCRIPTOR_COUNT=$((DESCRIPTOR_COUNT + n))
done

# --- what this run read ------------------------------------------------------
echo "    $SOURCE_COUNT source files under App/, $REGISTRAR_COUNT of them register a card"
echo "    $DESCRIPTOR_COUNT card descriptors graded, $REGISTRATION_COUNT Cards::registerCard() calls, $ASSIGNMENT_COUNT listingId assignments"

if [ "$REGISTRAR_COUNT" -eq 0 ]; then
  echo "ERROR: no file under App/ calls Cards::registerCard, so this gate checked nothing." >&2
  exit 1
fi

if [ "$SOURCE_COUNT" -lt 97 ]; then
  echo "ERROR: only $SOURCE_COUNT source files were swept under App/ and there were 97 on
       2026-11-03. Either files were removed, or the find above stopped matching and the
       grading below ran over less than App/ holds." >&2
  FAIL=1
fi
if [ "$REGISTRAR_COUNT" -lt 16 ]; then
  echo "ERROR: only $REGISTRAR_COUNT registering files were found and there were 16 on
       2026-11-03. Either cards were removed, or the search for Cards::registerCard
       stopped matching and this gate is checking less than it reads." >&2
  FAIL=1
fi
if [ "$DESCRIPTOR_COUNT" -lt 17 ]; then
  echo "ERROR: only $DESCRIPTOR_COUNT card descriptors were graded and there were 17 on
       2026-11-03. Either cards were removed, or the declaration patterns stopped matching
       and every grade below was handed out on a shorter list than the files hold." >&2
  FAIL=1
fi
if [ "$REGISTRATION_COUNT" -lt 17 ]; then
  echo "ERROR: only $REGISTRATION_COUNT Cards::registerCard() calls were found and there
       were 17 on 2026-11-03. Either cards were removed, or the count stopped matching,
       which takes the census this gate grades against with it." >&2
  FAIL=1
fi

# The one figure a hidden descriptor cannot leave alone. Becoming a card means
# being handed to Cards::registerCard(), so a descriptor the declaration patterns
# above cannot see still raises this count, and the two part company here.
if [ "$REGISTRATION_COUNT" != "$DESCRIPTOR_COUNT" ]; then
  echo "ERROR: $REGISTRATION_COUNT Cards::registerCard() calls against $DESCRIPTOR_COUNT
       descriptors this gate could name. Every registration has to name a descriptor
       declared as 'Cards::CardSpec <name>;', and every descriptor has to be registered by
       its own name, so these two figures are the same number or something registered here
       was graded by nothing. The per-file errors below name which." >&2
  FAIL=1
fi

if [ "$STRAY_COUNT" -ne 0 ]; then
  for file in $(printf '%s\n' "${!FILE_STRAYS[@]}" | sort); do
    if [ "${FILE_STRAYS[$file]}" -ne 0 ]; then
      say "App/$file" "reaches registerCard() ${FILE_STRAYS[$file]} time(s) other than as
           Cards::registerCard(), as Cards.h's declaration, or as CardManager.cpp's
           definition. A call arriving through a using declaration or a function pointer
           registers a card that no list here names and no grade below reaches. Call it as
           Cards::registerCard(<descriptor>)."
    fi
  done
fi

for file in $REGISTRARS; do
  SPECS=${SPECS_OF[$file]-}
  ARGS=${ARGS_OF[$file]-}

  if [ -z "${SPECS// /}" ]; then
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

  # Every registration names a descriptor this gate graded. This is where a
  # declaration spelling the patterns above do not recognise comes out: the
  # registration still has to name the thing, and the name lands nowhere.
  for arg in $ARGS; do
    if in_list "$arg" "$SPECS"; then
      continue
    fi
    # The argument is read as far as the first closing parenthesis, so anything
    # built in the call arrives here with its own punctuation attached and gets
    # the message that fits it.
    case "$arg" in
      [A-Za-z_]*)
        if [ -z "${arg//[A-Za-z0-9_]/}" ]; then
          say "App/$file" "hands '$arg' to Cards::registerCard() and this gate cannot see
               where '$arg' was declared as a Cards::CardSpec, so that card was graded by
               nothing. The descriptors it can name here are ($(echo $SPECS)). Declare it
               as 'Cards::CardSpec $arg;' and fill it in, the way every other card module
               here does."
          continue
        fi
        ;;
    esac
    say "App/$file" "registers a Cards::CardSpec built in the call itself, as
         'Cards::registerCard($arg...'. A descriptor with no name of its own is graded by
         nothing here, because every check below is per descriptor and reaches one through
         its name. Declare it as 'Cards::CardSpec <name>;', fill it in, and hand that name
         to Cards::registerCard()."
  done

  # Every listingId assignment in the file lands on a descriptor named above.
  # The count is the pin rather than the spelling: an assignment through a
  # reference, a pointer or a dereference reads as a resolver to the compiler and
  # names no card here, so the two tallies part company and this fails.
  ASSIGNED_ANYWHERE=${FILE_ASSIGNED[$file]}
  ASSIGNED_TO_SPECS=0
  for spec in $SPECS; do
    ASSIGNED_TO_SPECS=$((ASSIGNED_TO_SPECS + ${SPEC_ASSIGNED[$file/$spec]-0}))
  done
  if [ "$ASSIGNED_ANYWHERE" != "$ASSIGNED_TO_SPECS" ]; then
    say "App/$file" "has $ASSIGNED_ANYWHERE listingId assignment(s) and only
         $ASSIGNED_TO_SPECS of them land on a Cards::CardSpec this gate can name
         ($(echo $SPECS)). A resolver assigned through a reference, a pointer or a
         dereference reaches the card and tells nobody which card got one. Assign it on
         the descriptor by its own name."
  fi

  for spec in $SPECS; do
    if ! in_list "$spec" "$ARGS"; then
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
    if [ "${SPEC_ASSIGNED[$file/$spec]-0}" -gt 0 ] \
       && [ "${SPEC_NULLED[$file/$spec]-0}" -eq 0 ]; then
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
