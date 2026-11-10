#!/usr/bin/env bash
# Holds which cards owe a content key, which is the half of the decision no
# compiler on a runner can reach.
#
# A draw's content key is what the server keys a quarter-hour period row on -
# CARD_IMPRESSIONS_DESIGN.md, "Which attribute says what the draw was about". A
# card that reports none collapses every item it draws into one row, so a report
# can say the card was up 512 times and cannot say which thing was on the glass.
#
# --- what this adds to the compiler's half ----------------------------------
#
# Cards::registerCard() takes a Cards::KeyPolicy with no default, so every
# registration states what the card's draws report and one that states nothing
# does not compile - ci/test-key-policy.sh is that half. registerCard() then
# checks the policy stated against the descriptor's own listingId when the
# registration runs, through Cards::keyPolicyHolds(), and refuses a
# disagreement.
#
# Neither of those says the policy stated is the right answer for that card.
# Keyless on a card that walks a feed is self-consistent: it states there is
# nothing to name, it assigns no resolver, keyPolicyHolds() agrees with it, and
# every other gate passes. That is the shape App/Sports.cpp's sportslive was for
# 907 draws and 161 period rows, counted on 2026-11-03, out of a file whose three
# other cards reported a key on every draw.
#
# Which cards owe a key is a judgement about what a card puts on the glass rather
# than a property of the code that puts it there, so it is written down. The
# census below is that judgement, per card id, and this gate compares it against
# App/ both ways.
#
# --- why it is keyed on the card and not on the file ------------------------
#
# ci/check-content-key-resolvers.sh carried this as three lists of FILENAMES
# until 2026-11-03, and that is exactly how sportslive hid: Sports.cpp was in the
# list that resolves, on the strength of the three cards that do, while the
# fourth descriptor in the same file assigned nothing. A list keyed on the card id
# has no such strength to lend.
#
# --- what has to move together ----------------------------------------------
#
# Editing the census alone fails the per-module comparison. Editing a
# registration alone fails the same comparison. Only both together pass, and the
# two edits land in different files where a review reads them as one decision.
# Adding a card fails on an id the census does not name, whatever policy the
# registration states, so stating Keyless is no way past it.
#
# --- the limit, checked rather than assumed ---------------------------------
#
# The policy comparison is per module and not per descriptor: Announcement.cpp
# registers five cards from one templated call site and Sports.cpp registers four
# from two, so there is no registration to pair each id with. It therefore rests
# on the cards a module registers owing one policy between them, which all
# sixteen do today and which this gate fails on rather than assuming. A module
# whose cards owed two would let the set comparison pass on a swap.
#
# --- why it is a text scan --------------------------------------------------
#
# App/CardManager.cpp names a graphics library through Display.h's drawing path
# and does not compile on a host, so nothing in CI can walk the registry. What is
# read here is string literals and one argument token apiece rather than C++
# declarations, which is a narrower job than the retired lists had, and every
# figure prints with a floor under it so a pattern that stops matching fails
# instead of passing on nothing.
#
# BOOT_BUTTON_AND_IMPRESSIONS_DESIGN.md, "Which card owes which policy", is the
# record.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

# Set by the two readings of App/Cards.h below and by the grading at the bottom,
# so a disagreement about the enum is reported with everything else rather than
# ending the run before the figures print.
FAIL=0

# --- the census -------------------------------------------------------------
#
# One row per card id this firmware registers: the id, the policy it owes, and
# the module that defines it. Reasons by family, which are the table in
# BOOT_BUTTON_AND_IMPRESSIONS_DESIGN.md read down this column:
#
#   Resolves      The module is the only thing that can name what it drew. Most
#                 of these walk a feed and only the module knows which item is
#                 up. announcement1-5 and homevalue are here for a narrower
#                 reason - a notice's words arrive on the card policy and a
#                 valuation's address on the check-in response, and neither
#                 lands in assetId or qrData, which are the only two fields
#                 CardManager::noteShowing() falls back to. aircraft names the
#                 airframe's registration, which belongs to the aeroplane for
#                 its life, and falls back to the callsign.
#
#   PolicyContent The content arrives on the card policy, so noteShowing()'s
#                 assetId and qrData fallbacks answer and a resolver would
#                 repeat what is already in front of it. graphic draws the asset
#                 the policy names; qrtext encodes the payload it carries.
#
#   Keyless       Nothing to name beyond the card itself. A clock, a moon phase,
#                 a tide table, a forecast, a flyover and a day's events are
#                 about a place and a time rather than a thing, and an empty key
#                 is the state the period row has for them.
#
# homevalue and announcement1-5 were Keyless until 2026-11-03, reporting a
# summary on every draw and no key. The owner's call that day is that both owe
# one. CARD_IMPRESSIONS_DESIGN.md, "What a home-value and an announcement draw
# report", is the record.
CENSUS=$(
  cat <<'EOF'
aircraft         Resolves       Aircraft.cpp
announcement     Resolves       Announcement.cpp
announcement2    Resolves       Announcement.cpp
announcement3    Resolves       Announcement.cpp
announcement4    Resolves       Announcement.cpp
announcement5    Resolves       Announcement.cpp
featuredlistings Resolves       FeaturedListings.cpp
homevalue        Resolves       HomeValue.cpp
listings         Resolves       Listings.cpp
personalphotos   Resolves       PersonalPhotos.cpp
sportsteam       Resolves       Sports.cpp
sportsteam2      Resolves       Sports.cpp
sportsscores     Resolves       Sports.cpp
sportslive       Resolves       Sports.cpp
sportsteam3      Resolves       Sports.cpp
sportsteam4      Resolves       Sports.cpp
sportsteam5      Resolves       Sports.cpp
sportsscores2    Resolves       Sports.cpp
sportsscores3    Resolves       Sports.cpp
sportsscores4    Resolves       Sports.cpp
sportsscores5    Resolves       Sports.cpp
graphic          PolicyContent  Graphic.cpp
graphic2         PolicyContent  Graphic.cpp
graphic3         PolicyContent  Graphic.cpp
graphic4         PolicyContent  Graphic.cpp
graphic5         PolicyContent  Graphic.cpp
qrtext           PolicyContent  QrText.cpp
qrtext2          PolicyContent  QrText.cpp
qrtext3          PolicyContent  QrText.cpp
qrtext4          PolicyContent  QrText.cpp
qrtext5          PolicyContent  QrText.cpp
calendar         Keyless        Calendar.cpp
clockdate        Keyless        ClockDate.cpp
forecast         Keyless        Forecast.cpp
forecast2        Keyless        Forecast.cpp
forecast3        Keyless        Forecast.cpp
forecast4        Keyless        Forecast.cpp
forecast5        Keyless        Forecast.cpp
issflyover       Keyless        IssFlyover.cpp
moonphase        Keyless        MoonPhase.cpp
sunmoon          Keyless        SunMoon.cpp
tides            Keyless        Tides.cpp
EOF
)

# --- the policies App/Cards.h actually offers --------------------------------
#
# Read out of the enum rather than written here, so a fourth value added to
# Cards.h stops this gate until the census says which cards owe it. A census
# naming a value the enum does not hold fails the same comparison from the other
# side, which is what a renamed policy looks like.
KNOWN_POLICIES=$(
  sed 's://.*::' App/Cards.h | tr -d ' \t\r\n' |
    grep -o 'enumclassKeyPolicy:uint8_t{[^}]*}' |
    sed -e 's/^.*{//' -e 's/}$//' | tr ',' '\n' | grep -E '^[A-Za-z_][A-Za-z0-9_]*$' | sort -u || true
)
KNOWN_POLICY_COUNT=$(printf '%s\n' $KNOWN_POLICIES | grep -c . || true)

if [ "$KNOWN_POLICY_COUNT" -eq 0 ]; then
  echo "ERROR: App/Cards.h: 'enum class KeyPolicy : uint8_t { ... }' was not found, so this" >&2
  echo "       gate read no policy names and has nothing to grade the census against." >&2
  exit 1
fi

CENSUS_POLICIES_USED=$(awk 'NF >= 2 { print $2 }' <<<"$CENSUS" | grep -E '^[A-Za-z_]' | sort -u)

if [ "$KNOWN_POLICIES" != "$CENSUS_POLICIES_USED" ]; then
  echo "ERROR: App/Cards.h's KeyPolicy values and the policies the census owes are not the" >&2
  echo "       same set. Cards.h holds:" >&2
  printf '%s\n' $KNOWN_POLICIES | sed 's/^/         /' >&2
  echo "       the census owes:" >&2
  printf '%s\n' $CENSUS_POLICIES_USED | sed 's/^/         /' >&2
  echo "       A value in the enum that no card owes needs a census row saying which card" >&2
  echo "       does, or the enum does not need it. A census row naming a value the enum" >&2
  echo "       does not hold grades against nothing." >&2
  FAIL=1
fi

# --- the census against the registry's own size ------------------------------
#
# Cards::kMaxCards is kept exactly equal to the number of registrations - read
# its remarks in App/Cards.h, which account for the count card by card and say
# why the registry is deliberately not sized with room to spare. So a new card
# means bumping that line, and comparing it with the census is what catches a
# card whose id this gate's patterns did not read: the two part company and the
# gate fails rather than grading a shorter list.
MAX_CARDS=$(
  sed 's://.*::' App/Cards.h | tr -d ' \t\r\n' |
    grep -o 'kMaxCards=[0-9]*' | head -1 | sed 's/^kMaxCards=//' || true
)

if [ -z "$MAX_CARDS" ]; then
  echo "ERROR: App/Cards.h: 'static constexpr uint8_t kMaxCards = N;' was not found, so the" >&2
  echo "       census could not be compared with the size of the registry it describes." >&2
  exit 1
fi

# The comparison itself is in the grading at the bottom, which counts the census
# rows as it reads them.

# --- what App/ says ---------------------------------------------------------
#
# Every source file under App/, recursively, headers as well as translation
# units, because a registration reaches the registry from wherever it is written.
# App/build holds generated copies of these same files and is pruned: it is
# gitignored output, and reading it would grade a stale duplicate of every card.
mapfile -t SOURCES < <(find App -type d -name build -prune -o -type f \
  \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' -o -name '*.c' -o -name '*.ino' \) \
  -print | sort)

SOURCE_COUNT=${#SOURCES[@]}

if [ "$SOURCE_COUNT" -eq 0 ]; then
  echo "ERROR: no source file was found under App/, so this gate read nothing." >&2
  exit 1
fi

# The files the character scan below runs over, narrowed to the ones naming the
# registry at all. On the unqualified name rather than on `Cards::registerCard(`,
# so a call written with the qualifier or the parenthesis on another line is still
# a candidate, and so are App/Cards.h and App/CardManager.cpp, which declare and
# define it. The scan itself is what decides whether a candidate registers
# anything: a file naming it only in a remark reports nothing, because the
# comments come out before any pattern runs.
#
# This is the difference between reading 400KB and reading every byte under App/,
# which costs two minutes on App/Display.cpp alone.
mapfile -t CANDIDATES < <(printf '%s\n' "${SOURCES[@]}" | xargs grep -l 'registerCard' 2>/dev/null | sort)

CANDIDATE_COUNT=${#CANDIDATES[@]}

if [ "$CANDIDATE_COUNT" -eq 0 ]; then
  echo "ERROR: nothing under App/ names registerCard, so this gate read nothing. Either the" >&2
  echo "       registry was renamed, or the sweep above stopped reading App/." >&2
  exit 1
fi

# One pass over every candidate, reporting what each holds as tab-separated records.
#
# Comments come out first, block as well as line, as a scan carrying its state
# across lines: these files explain their own registrations in remarks that quote
# them, and a fixed-string search cannot tell a statement from a commented copy
# of one. \r goes with the rest of the whitespace, because these files are
# checked out CRLF under core.autocrlf and a carriage return inside a joined
# statement stops every pattern below from matching.
#
# What is left is the file as one run of characters with no whitespace at all, so
# a statement that wraps across lines reads the same as one that fits and
# `kCardId2 = "x"` reads the same as `kCardId2="x"`. A string literal holding a
# space is joined up by this too; no card id has one, and a mangled literal
# cannot produce an id the census would accept.
# A WALL CLOCK ON THE PARSE, because a gate that hangs is worse than one that
# fails. The four match() loops below advance `rest` by hand, and one that does
# not advance finds the same match for ever while allocating a new `piece` every
# pass - an infinite loop that eats memory rather than spinning quietly.
#
# That is not a hypothetical. Two copies of a DRAFT of this script, started
# 2026-11-03 at 21:49 and 21:52 and never committed, were still running two days
# later at 6.3GB and 7.8GB. Together they starved the production service until it
# could not answer its own 30-second start timeout, and nothing noticed for two
# days because nothing was watching and the script could not stop itself.
#
# 120 seconds against the 1-2 seconds this takes on 99 files. A kill exits
# non-zero, `set -euo pipefail` carries that out, and the gate fails - which is
# a red check somebody reads, instead of a machine somebody has to rescue.
RECORDS=$(timeout 120 awk '
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

  function report(path,    rel, regs, pos, q, rest, k, inner, piece, id, s, pfx, adv) {
    rel = path
    sub(/^App\//, "", rel)

    regs = countOf(TIGHT, "Cards::registerCard(")
    if (regs == 0) { return }
    printf "REG\t%s\t%d\n", rel, regs

    # The policy each registration states, read out of that call argument list
    # and no further. A call whose policy token this cannot find is reported as
    # the text it read, so the message can quote it rather than grade a
    # registration it could not place.
    pfx = "Cards::KeyPolicy::"
    pos = 1
    while (1) {
      q = index(substr(TIGHT, pos), "Cards::registerCard(")
      if (q == 0) { break }
      pos = pos + q - 1 + length("Cards::registerCard(")
      rest = substr(TIGHT, pos)
      k = index(rest, ")")
      if (k == 0) { inner = rest } else { inner = substr(rest, 1, k - 1) }
      if (match(inner, /Cards::KeyPolicy::[A-Za-z_][A-Za-z0-9_]*/)) {
        printf "POL\t%s\t%s\n", rel, \
          substr(inner, RSTART + length(pfx), RLENGTH - length(pfx))
      } else {
        printf "UNREAD\t%s\t%s\n", rel, inner
      }
    }

    # The ids a module defines, in the three shapes App/ writes them.
    #
    # Each loop takes its own next position out of RSTART and RLENGTH the moment
    # the match lands and before anything else runs. sub() and a nested match()
    # both overwrite those two, and reading them after the body is what sent the
    # third loop below round forever on Sports.cpp: the inner match() ran out,
    # left RSTART 0 and RLENGTH -1, and substr(rest, -1) handed back the whole
    # string again.
    #
    # `kCardId = "clockdate"`, and the numbered constants the four five-instance
    # modules carry beside it.
    rest = TIGHT
    while (match(rest, /kCardId[A-Za-z0-9_]*="[^"]*"/)) {
      piece = substr(rest, RSTART, RLENGTH)
      adv = RSTART + RLENGTH
      sub(/^kCardId[A-Za-z0-9_]*="/, "", piece)
      sub(/"$/, "", piece)
      if (piece != "") { printf "CID\t%s\t%s\n", rel, piece }
      rest = substr(rest, adv)
    }

    # A literal written straight onto the descriptor, which is what
    # Aircraft.cpp, Listings.cpp and FeaturedListings.cpp do.
    rest = TIGHT
    while (match(rest, /\.id="[^"]*"/)) {
      piece = substr(rest, RSTART, RLENGTH)
      adv = RSTART + RLENGTH
      sub(/^\.id="/, "", piece)
      sub(/"$/, "", piece)
      if (piece != "") { printf "CID\t%s\t%s\n", rel, piece }
      rest = substr(rest, adv)
    }

    # The parallel array Sports.cpp keeps its four ids in, so that they sit
    # beside the slots they index rather than at each registration.
    rest = TIGHT
    while (match(rest, /kCardId[A-Za-z0-9_]*\[[^]]*\]=\{[^}]*\}/)) {
      piece = substr(rest, RSTART, RLENGTH)
      adv = RSTART + RLENGTH
      sub(/^[^{]*\{/, "", piece)
      sub(/\}$/, "", piece)
      s = piece
      while (match(s, /"[^"]*"/)) {
        id = substr(s, RSTART + 1, RLENGTH - 2)
        s = substr(s, RSTART + RLENGTH)
        if (id != "") { printf "CID\t%s\t%s\n", rel, id }
      }
      rest = substr(rest, adv)
    }
  }

  FILENAME != current {
    if (current != "") { report(current) }
    current = FILENAME
    TIGHT = ""
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

    gsub(/[ \t]/, "", code)
    TIGHT = TIGHT code
  }

  END {
    if (current != "") { report(current) }
  }
' "${CANDIDATES[@]}")


# --- the grading ------------------------------------------------------------
#
# One pass over the census rows and the records above together, rather than a
# shell loop per comparison. Sixteen modules against five comparisons apiece
# came to a hundred subshells and thirty-seven seconds on the workstation this
# firmware is written on, where starting a process costs what a page of awk
# costs.
#
# Everything comes back on stdout and the loop at the bottom puts the lines
# marked '!' on stderr, rather than awk writing to "/dev/stderr" itself: nothing
# else under ci/ does that, this runs under gawk here and under whatever awk the
# runner has, and a gate is a poor place to find out which ones open that path.
GRADE_OUT=$(mktemp)
trap 'rm -f "$GRADE_OUT"' EXIT

{
  awk 'NF >= 3 { printf "CENSUS\t%s\t%s\t%s\n", $3, $1, $2 }' <<<"$CENSUS"
  printf '%s\n' "$RECORDS"
} | awk -F'\t' \
  -v max_cards="$MAX_CARDS" \
  -v known_policies="$KNOWN_POLICY_COUNT" \
  -v source_count="$SOURCE_COUNT" \
  -v candidate_count="$CANDIDATE_COUNT" '
  function inList(list, item) {
    return index(" " list " ", " " item " ") > 0
  }

  # Insertion sort over a space-separated list, so every message names its ids
  # and its policies in one order whatever order they were read in. The lists
  # are five long at most.
  function sorted(list,   n, a, i, j, t, out) {
    n = split(list, a, " ")
    for (i = 2; i <= n; i++) {
      t = a[i]
      j = i - 1
      while (j >= 1 && a[j] > t) { a[j + 1] = a[j]; j-- }
      a[j + 1] = t
    }
    out = ""
    for (i = 1; i <= n; i++) { out = (out == "" ? a[i] : out " " a[i]) }
    return out
  }

  function uniq(list,   s, n, a, i, out) {
    s = sorted(list)
    n = split(s, a, " ")
    out = ""
    for (i = 1; i <= n; i++) {
      if (i == 1 || a[i] != a[i - 1]) { out = (out == "" ? a[i] : out " " a[i]) }
    }
    return out
  }

  function words(list,   a) {
    return split(list, a, " ")
  }

  # Each line of a message carries the marker, so a message that runs to four
  # lines reaches stderr as four and not as one line and three loose ones.
  function fail(subject, message,   n, a, i) {
    n = split(message, a, "\n")
    printf "!ERROR: %s: %s\n", subject, a[1]
    for (i = 2; i <= n; i++) { printf "!%s\n", a[i] }
    failures++
  }

  function floor(what, got, want) {
    if (got < want) {
      fail("what this run read", what " came to " got " and there were " want " on\n" \
        "       2026-11-03. Either cards were retired, or a pattern above stopped matching\n" \
        "       and this gate graded less than App/ holds.")
    }
  }

  $1 == "CENSUS" {
    module = $2; id = $3; policy = $4
    if (id in censusModule) {
      fail("the census", "names the card id " id " twice, so one of the two rows describes\n" \
        "       nothing.")
      next
    }
    censusModule[id] = module
    censusPolicy[id] = policy
    censusIds[module] = censusIds[module] " " id
    censusPols[module] = censusPols[module] " " policy
    if (!inList(censusModules, module)) { censusModules = censusModules " " module }
    censusCount++
    next
  }

  $1 == "REG" {
    discRegs[$2] = $3
    if (!inList(discModules, $2)) { discModules = discModules " " $2 }
    regTotal += $3
    next
  }

  $1 == "POL" {
    discPols[$2] = discPols[$2] " " $3
    statedTotal++
    next
  }

  $1 == "CID" {
    if (!inList(discIds[$2], $3)) {
      discIds[$2] = discIds[$2] " " $3
      idTotal++
    }
    next
  }

  $1 == "UNREAD" {
    fail("App/" $2, "a Cards::registerCard() call states a key policy this gate could not\n" \
      "       read. It handed over [" $3 "]. Every registration has to name\n" \
      "       Cards::KeyPolicy::<value> in that argument list, because the policy stated is\n" \
      "       what the census is compared against.")
    next
  }

  END {
    censusModuleCount = words(censusModules)
    discModuleCount = words(discModules)

    if (discModuleCount == 0) {
      fail("App/", "no file calls Cards::registerCard(), so this gate graded nothing. Either\n" \
        "       the registry was renamed, or the scan above stopped reading App/.")
      exit 1
    }

    if (censusCount != max_cards) {
      fail("the census", "holds " censusCount " card ids and Cards::kMaxCards is " max_cards ".\n" \
        "       That line is kept exactly equal to the number of registrations - read its own\n" \
        "       remarks in App/Cards.h - so the two disagreeing means a card was added or\n" \
        "       retired on one side only. A card added needs a census row saying what its\n" \
        "       draws report, and stating Keyless at the registration is no way past this.")
    }

    # --- the modules, both ways ---------------------------------------------
    n = split(discModules, dm, " ")
    for (i = 1; i <= n; i++) {
      if (!(dm[i] in censusIds)) {
        fail("App/" dm[i], "registers a card and the census names it nowhere. Every card this\n" \
          "       firmware registers owes a row saying what its draws report - Resolves,\n" \
          "       PolicyContent or Keyless - and a new module is a new group of rows.")
      }
    }

    n = split(censusModules, cm, " ")
    for (i = 1; i <= n; i++) {
      if (!(cm[i] in discRegs)) {
        fail("the census", "names App/" cm[i] " and nothing in it calls Cards::registerCard().\n" \
          "       Either the module stopped registering cards and its rows describe nothing, or\n" \
          "       the file moved and this gate is reading less than App/ holds.")
      }
    }

    # --- the ids, both ways, and the policies against them ------------------
    for (i = 1; i <= n; i++) {
      module = cm[i]
      if (!(module in discRegs)) { continue }

      want = uniq(censusIds[module])
      got = uniq(discIds[module])

      if (got == "") {
        fail("App/" module, "registers " discRegs[module] " card(s) and this gate read no card id\n" \
          "       out of it, while the census names " words(want) ". An id is written as a kCardId\n" \
          "       constant, as a literal straight onto the descriptor, or in the kCardIds array;\n" \
          "       a fourth shape is one this gate cannot grade.")
      } else if (want != got) {
        split(want, wa, " ")
        split(got, ga, " ")
        extra = ""
        missing = ""
        for (j = 1; j <= words(got); j++) {
          if (!inList(want, ga[j])) { extra = extra " " ga[j] }
        }
        for (j = 1; j <= words(want); j++) {
          if (!inList(got, wa[j])) { missing = missing " " wa[j] }
        }
        message = "the card ids App/" module " defines are not the ids the census names for it."
        if (extra != "") {
          message = message "\n       Defined and not censused:" extra ". Give each one a row saying what" \
            "\n       its draws report."
        }
        if (missing != "") {
          message = message "\n       Censused and not defined:" missing ". Those rows describe nothing."
        }
        fail("App/" module, message)
      }

      # The invariant the policy comparison rests on, checked rather than
      # assumed. A module registering several cards from one call site has no
      # registration to pair each id with, so the comparison is per module, and
      # a module owing two policies would let it pass on a swap.
      owed = uniq(censusPols[module])
      if (words(owed) > 1) {
        fail("the census", "owes App/" module " " words(owed) " different policies across " \
          words(censusIds[module]) " cards.\n" \
          "       The policy comparison is per module, because a module can register several\n" \
          "       cards from one call site and this gate cannot tell which registration belongs\n" \
          "       to which card. Grading has to become per descriptor before a module owing two\n" \
          "       policies lands.")
      }

      if (discPols[module] == "") {
        fail("App/" module, "calls Cards::registerCard() " discRegs[module] " time(s) and this gate read\n" \
          "       no policy out of any of them, so nothing was graded against the census.")
        continue
      }

      stated = uniq(discPols[module])
      if (owed != stated) {
        fail("App/" module, "states " stated " where the census owes " owed " for\n" \
          "       " uniq(censusIds[module]) ".\n" \
          "       Either the registration is wrong about what those draws report, or the census\n" \
          "       is, and whichever it is the other one has to be changed with it.")
      }
    }

    # --- how much it read --------------------------------------------------
    #
    # Every figure is one this run measured, and the floors are the 2026-11-03
    # readings. A gate whose patterns stopped matching passes by finding
    # nothing, and these counts are the only way to see that from the outside,
    # so they print whether or not anything failed.
    printf "    %d source files under App/, %d naming the registry, %d registering a card\n", \
      source_count, candidate_count, discModuleCount
    printf "    %d card ids censused against kMaxCards %d, %d read out of App/\n", \
      censusCount, max_cards, idTotal
    printf "    %d Cards::registerCard() calls, %d policies stated, %d in the enum\n", \
      regTotal, statedTotal, known_policies

    floor("the source files swept under App/", source_count, 97)
    floor("the files naming the registry", candidate_count, 18)
    floor("the modules registering a card", discModuleCount, 16)
    floor("the modules in the census", censusModuleCount, 16)
    floor("the census rows", censusCount, 35)
    floor("the card ids read out of App/", idTotal, 35)
    floor("the Cards::registerCard() calls", regTotal, 17)
    floor("the policies stated at those calls", statedTotal, 17)
    floor("the KeyPolicy values in App/Cards.h", known_policies, 3)

    if (failures > 0) { exit 1 }
  }
' >"$GRADE_OUT" || FAIL=1

# The marked lines to stderr and the figures to stdout, in the order awk wrote
# them. A bash loop rather than a grep pair, so the figures cannot go missing
# because a grep found nothing and the pipeline took its status.
while IFS= read -r line; do
  case "$line" in
    '!'*) printf '%s\n' "${line#!}" >&2 ;;
    *) printf '%s\n' "$line" ;;
  esac
done <"$GRADE_OUT"

if [ "$FAIL" -ne 0 ]; then
  echo "       See BOOT_BUTTON_AND_IMPRESSIONS_DESIGN.md, 'Which card owes which policy'," >&2
  echo "       CARD_IMPRESSIONS_DESIGN.md in the server tree, 'Which attribute says what the" >&2
  echo "       draw was about', and the remarks at the top of this script." >&2
  exit 1
fi

echo "==> every card App/ registers owes the policy its registration states"
