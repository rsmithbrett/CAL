#!/usr/bin/env bash
# Writes the card ids this App build registers into App/CardManifest.g.h, and
# with --check fails when the committed file is not what a fresh reading of
# App/ produces.
#
# --- what this is for -------------------------------------------------------
#
# The server keeps a catalog of card types and a per-entry flag saying whether
# firmware registers that id. Until now the flag's evidence was a list somebody
# read out of this repository by hand and pasted into the server tree, with the
# release it was read from written beside it as a version string. A version
# string is not evidence: on 2026-11-09 seven sports ids were pasted in against
# a branch of this repo that no release carried, every test on that side stayed
# green, and the server grew seven card targets nothing draws.
#
# What the server CAN read is the image it stores for a release. The ids are
# already in there as the string literals the registrations pass, so an id the
# catalog claims and the image does not hold is already catchable. What the
# image does not carry is the LIST - a reader can confirm an id it already
# suspects and cannot enumerate what it has never heard of. This file puts the
# list itself into the image, as one contiguous run of bytes between two
# markers, so the server can read out what a build registers rather than
# checking its own guesses one at a time.
#
# The manifest therefore travels inside the artifact it describes. There is no
# second file to publish, no release asset to go missing, and no row for an
# operator to fill in: a device's App image and the list of cards it registers
# cannot be separated, because they are the same bytes.
#
# --- how the ids are derived ------------------------------------------------
#
# Out of App/ by text scan, in the three shapes the registrations write an id:
#
#   kCardId = "clockdate"                    a module's own constant
#   .id = "listings"                         a literal straight onto the spec
#   kCardIds[] = { "sportsteam", ... }       Sports.cpp's parallel table
#
# The same three shapes ci/check-card-key-decisions.sh reads, by the same
# comment-stripping scan, for the same reason it gives: App/CardManager.cpp
# names a graphics library and does not compile on a host, so nothing in CI can
# walk the registry itself.
#
# A text scan can read less than App/ holds, and a scan that reads less writes a
# shorter manifest rather than failing. Two things stop that being quiet:
#
#   1. The count is compared with Cards::kMaxCards, which App/Cards.h keeps
#      exactly equal to the number of registrations and accounts for card by
#      card in its own remarks. A pattern that stopped matching parts company
#      with that line.
#   2. A floor, the 2026-11-10 reading, under the count of ids and the count of
#      modules they came from.
#
# --- what reads the manifest ------------------------------------------------
#
# App/CardManager.cpp logs it at begin(), which is also what keeps the literal
# in the image: a string nothing references can be dropped at link time, and a
# manifest the linker removed would read on the server exactly like a release
# published before this existed.
#
# The server's side is FirmwareSnapshotReleaseTests in the DiscoverAroundMe
# tree, which reads the stored image for the release its catalog names. The
# marker and the version prefix below are what the two sides agree on; see the
# remarks on KnownCardCatalog.FirmwareRegisteredIdSnapshot there, and the
# "The ids a build registers, written into the image" part of this repo's
# README.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

HEADER="App/CardManifest.g.h"
CHECK=0
if [ "${1:-}" = "--check" ]; then
  CHECK=1
elif [ -n "${1:-}" ]; then
  echo "usage: $(basename "$0") [--check]" >&2
  exit 2
fi

# THE MARKER AND THE VERSION ARE A CONTRACT WITH THE SERVER TREE. The reader
# there finds kManifestOpen, takes everything up to kManifestClose, and refuses a
# version it does not know - so this prefix moves only when both sides move. The
# ids are comma-separated with no spaces, sorted, and the whole thing is one
# string literal so it lands in the image as one run of bytes.
MARKER_OPEN="DAMCARDIDS1:"
MARKER_CLOSE=":ENDCARDIDS"

# --- what App/ says ---------------------------------------------------------
#
# App/build holds generated copies of these files and is pruned: it is
# gitignored output, and reading it would derive the manifest from a stale
# duplicate of every card.
mapfile -t SOURCES < <(find App -type d -name build -prune -o -type f \
  \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' -o -name '*.c' -o -name '*.ino' \) \
  -print | sort)

if [ "${#SOURCES[@]}" -eq 0 ]; then
  echo "ERROR: no source file was found under App/, so no card id was read." >&2
  exit 1
fi

# Narrowed to the files naming the registry at all, on the unqualified name, so a
# call written with the qualifier or the parenthesis on another line is still a
# candidate. The generated header is excluded by name: it holds every id as one
# literal and would read back as a module defining all of them.
mapfile -t CANDIDATES < <(printf '%s\n' "${SOURCES[@]}" \
  | grep -v "^${HEADER}\$" \
  | xargs grep -l 'registerCard' 2>/dev/null | sort)

if [ "${#CANDIDATES[@]}" -eq 0 ]; then
  echo "ERROR: nothing under App/ names registerCard, so no card id was read. Either the" >&2
  echo "       registry was renamed, or the sweep above stopped reading App/." >&2
  exit 1
fi

# One pass over the candidates, printing "module<TAB>id" records. Comments come
# out first, block as well as line, because these modules quote their own
# registrations in remarks and a fixed-string search cannot tell a statement from
# a commented copy of one. \r goes with the rest of the whitespace: these files
# are checked out CRLF under core.autocrlf and a carriage return inside a joined
# statement stops every pattern below matching.
#
# Each loop takes its next position out of RSTART and RLENGTH the moment the
# match lands, before anything else runs. sub() and a nested match() both
# overwrite those two, and reading them afterwards is what sent the equivalent
# loop in check-card-key-decisions.sh round for ever on Sports.cpp.
#
# A WALL CLOCK ON THE PARSE, for the reason that script records at length: two
# copies of a draft of it ran for two days at 6.3GB and 7.8GB and starved the
# production service. A kill exits non-zero and set -e carries that out.
RECORDS=$(timeout 120 awk '
  function report(path,    rel, rest, piece, s, id, adv) {
    rel = path
    sub(/^App\//, "", rel)
    if (index(TIGHT, "Cards::registerCard(") == 0) { return }

    # kCardId = "clockdate", and the numbered constants the five-instance
    # modules carry beside it.
    rest = TIGHT
    while (match(rest, /kCardId[A-Za-z0-9_]*="[^"]*"/)) {
      piece = substr(rest, RSTART, RLENGTH)
      adv = RSTART + RLENGTH
      sub(/^kCardId[A-Za-z0-9_]*="/, "", piece)
      sub(/"$/, "", piece)
      if (piece != "") { printf "%s\t%s\n", rel, piece }
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
      if (piece != "") { printf "%s\t%s\n", rel, piece }
      rest = substr(rest, adv)
    }

    # The parallel array Sports.cpp keeps its ids in, so they sit beside the
    # slots they index rather than at each registration.
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
        if (id != "") { printf "%s\t%s\n", rel, id }
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

  END { if (current != "") { report(current) } }
' "${CANDIDATES[@]}")

IDS=$(printf '%s\n' "$RECORDS" | awk -F'\t' 'NF == 2 { print $2 }' | sort -u)
MODULES=$(printf '%s\n' "$RECORDS" | awk -F'\t' 'NF == 2 { print $1 }' | sort -u)
ID_COUNT=$(printf '%s\n' "$IDS" | grep -c . || true)
MODULE_COUNT=$(printf '%s\n' "$MODULES" | grep -c . || true)

# --- the ids against the registry's own size --------------------------------
#
# Cards::kMaxCards is kept exactly equal to the number of registrations - read
# its remarks in App/Cards.h - so the two parting company means a card was added
# or retired on one side only, or that a pattern above stopped matching.
MAX_CARDS=$(
  sed 's://.*::' App/Cards.h | tr -d ' \t\r\n' |
    grep -o 'kMaxCards=[0-9]*' | head -1 | sed 's/^kMaxCards=//' || true
)

if [ -z "$MAX_CARDS" ]; then
  echo "ERROR: App/Cards.h: 'static constexpr uint8_t kMaxCards = N;' was not found, so the" >&2
  echo "       ids read out of App/ could not be compared with the size of the registry they" >&2
  echo "       describe." >&2
  exit 1
fi

echo "    $ID_COUNT card ids read out of ${#CANDIDATES[@]} candidate files, $MODULE_COUNT modules, kMaxCards $MAX_CARDS"

FAIL=0

if [ "$ID_COUNT" -ne "$MAX_CARDS" ]; then
  echo "ERROR: $ID_COUNT card ids were read out of App/ and Cards::kMaxCards is $MAX_CARDS. That" >&2
  echo "       line is kept exactly equal to the number of registrations, so the two" >&2
  echo "       disagreeing means either a card was added or retired on one side only, or an id" >&2
  echo "       is written in a fourth shape this script cannot read. A manifest shorter than" >&2
  echo "       the build is a manifest the server grades its catalog against, so this does not" >&2
  echo "       write one." >&2
  FAIL=1
fi

# The 2026-11-10 reading, as floors. A scan whose patterns stopped matching
# passes by finding nothing, and these are the only way to see that from outside.
if [ "$ID_COUNT" -lt 35 ]; then
  echo "ERROR: the ids read out of App/ came to $ID_COUNT and there were 35 on 2026-11-10." >&2
  FAIL=1
fi
if [ "$MODULE_COUNT" -lt 16 ]; then
  echo "ERROR: the modules defining a card id came to $MODULE_COUNT and there were 16 on 2026-11-10." >&2
  FAIL=1
fi

for id in $IDS; do
  case "$id" in
    *[!a-z0-9]*)
      echo "ERROR: '$id' was read as a card id and the manifest holds lowercase letters and" >&2
      echo "       digits only. A comma or a marker inside an id would make the manifest" >&2
      echo "       unreadable from the other side." >&2
      FAIL=1
      ;;
  esac
done

if [ "$FAIL" -ne 0 ]; then
  echo "       See the remarks at the top of this script, and App/Cards.h's own account of" >&2
  echo "       kMaxCards." >&2
  exit 1
fi

MANIFEST="${MARKER_OPEN}$(printf '%s\n' "$IDS" | paste -sd, -)${MARKER_CLOSE}"

GENERATED=$(mktemp)
trap 'rm -f "$GENERATED"' EXIT

{
  cat <<EOF
// GENERATED by ci/write-card-manifest.sh. Do not edit by hand: the next run of
// that script overwrites it, and ci/build-firmware.sh runs the script with
// --check before compiling, so an edit here fails the build rather than
// shipping.
//
// Every card id this App build registers, as one string literal, so the server
// can read the list out of the image it stores for a release instead of
// trusting a list somebody pasted into its own tree. App/CardManager.cpp logs
// it at begin(), which is also what keeps it from being dropped at link time.
//
// $ID_COUNT ids from $MODULE_COUNT modules, against Cards::kMaxCards $MAX_CARDS.
#pragma once

namespace Cards {

inline constexpr char kCardIdManifest[] =
    "$MANIFEST";

}  // namespace Cards
EOF
} >"$GENERATED"

if [ "$CHECK" -eq 1 ]; then
  if [ ! -f "$HEADER" ]; then
    echo "ERROR: $HEADER is missing. Run 'bash ci/write-card-manifest.sh' and commit it." >&2
    exit 1
  fi
  # On the bytes, not on the ids: a header the compiler reads differently from
  # what this script writes is a header nobody regenerated.
  if ! diff -u "$HEADER" "$GENERATED" >/dev/null 2>&1; then
    echo "ERROR: $HEADER is not what a fresh reading of App/ produces. A card was added," >&2
    echo "       retired or renamed and the manifest was not regenerated, so the image would" >&2
    echo "       carry a list of ids that is not the list it registers." >&2
    echo "" >&2
    echo "       Run: bash ci/write-card-manifest.sh    and commit $HEADER." >&2
    echo "" >&2
    diff -u "$HEADER" "$GENERATED" >&2 || true
    exit 1
  fi
  echo "==> the card id manifest matches the registrations in App/"
  exit 0
fi

cp "$GENERATED" "$HEADER"
echo "==> wrote $HEADER"
printf '%s\n' "$MANIFEST"
