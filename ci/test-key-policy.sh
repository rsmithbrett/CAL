#!/usr/bin/env bash
# Holds the decision every card module has to make about what its draws report,
# in the one place every card goes through: Cards::registerCard()'s own
# signature.
#
# A draw's content key is what the server keys a quarter-hour period row on -
# CARD_IMPRESSIONS_DESIGN.md, "Which attribute says what the draw was about". A
# card that reports none collapses every item it draws into one row, so a report
# can say the card was up 512 times and cannot say which thing was on the glass.
#
# registerCard() takes a Cards::KeyPolicy with no default, so a registration
# that names no policy does not compile. Three runs here:
#
#   1. tests/key-policy-test.cpp runs Cards::keyPolicyHolds() over all three
#      policies against a descriptor with a resolver and one without, which is
#      the rule registerCard() applies once a registration runs.
#   2. tests/key-policy-signature.cpp compiles as every module writes the call.
#   3. The same file with KEY_POLICY_OMITTED defined, which drops the argument,
#      and the compiler has to refuse it.
#
# The third run is the one that replaced three lists of filenames in
# ci/check-content-key-resolvers.sh. A text scan over C++ graded declarations it
# recognised, and widening its patterns twice is what it cost; the compiler
# reads the call whatever the descriptor is called and wherever it sits.
#
# Compiled and not linked for runs 2 and 3: registerCard()'s definition is in
# App/CardManager.cpp, which names a graphics library and does not compile on a
# host, and a signature is a question for the front end anyway.
#
# App/Cards.h is compiled as it ships. tests/key-policy-stubs supplies the
# <Arduino.h> it includes.
#
# What this cannot check is that registerCard() still asks keyPolicyHolds(), and
# the fallback order CardManager::noteShowing() applies around it. Both are in
# the file that will not compile here, and ci/check-content-key-resolvers.sh is
# what holds them.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

TEST_BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_BUILD_DIR"' EXIT

CXX_BIN="${CXX:-g++}"
COMMON=(-std=c++17 -Wall -Wextra -Werror -Itests/key-policy-stubs -IApp)

echo "==> The key policy against the descriptor it was stated for"

"$CXX_BIN" "${COMMON[@]}" tests/key-policy-test.cpp -o "$TEST_BUILD_DIR/key-policy-test"

# The binary's own PASS line is what says it ran, rather than its exit status. A
# main() that returned before reaching an assertion exits 0 as happily as one
# that passed every case, and a gate reading the status cannot tell them apart.
OUTPUT=$("$TEST_BUILD_DIR/key-policy-test" || true)
echo "$OUTPUT"
case "$OUTPUT" in
  *"PASS: key policy against the descriptor it was stated for"*) ;;
  *)
    echo "ERROR: the key policy test finished without printing its PASS line." >&2
    exit 1
    ;;
esac

# --- the registration as it is written, and the same call without the policy --
#
# The object file is what says each run did what it was supposed to, rather than
# the exit status: a compiler that failed and exited 0 writes nothing, and one
# that was asked to refuse a call and compiled it anyway leaves the file behind.
echo "==> A registration naming its key policy"

STATED_OBJ="$TEST_BUILD_DIR/key-policy-signature.o"
STATED_LOG="$TEST_BUILD_DIR/stated.log"
set +e
"$CXX_BIN" "${COMMON[@]}" -c tests/key-policy-signature.cpp -o "$STATED_OBJ" \
  >"$STATED_LOG" 2>&1
set -e
cat "$STATED_LOG"
if [ ! -s "$STATED_OBJ" ]; then
  echo "ERROR: tests/key-policy-signature.cpp did not compile, so the two-argument form" >&2
  echo "       every card module in App/ now writes is not the form Cards.h declares." >&2
  exit 1
fi
echo "    compiled, $(wc -c <"$STATED_OBJ" | tr -d ' ') bytes of object"

echo "==> The same registration with the key policy left out"

OMITTED_OBJ="$TEST_BUILD_DIR/key-policy-omitted.o"
OMITTED_LOG="$TEST_BUILD_DIR/omitted.log"
set +e
"$CXX_BIN" "${COMMON[@]}" -DKEY_POLICY_OMITTED -c tests/key-policy-signature.cpp \
  -o "$OMITTED_OBJ" >"$OMITTED_LOG" 2>&1
set -e
sed 's/^/    /' "$OMITTED_LOG"

if [ -s "$OMITTED_OBJ" ]; then
  echo "ERROR: Cards::registerCard(spec) compiled with no key policy named. A card can" >&2
  echo "       reach the registry again without anybody deciding what its draws report," >&2
  echo "       which is the whole of what this change was for. Give registerCard()'s" >&2
  echo "       keyPolicy parameter no default value in App/Cards.h." >&2
  exit 1
fi

# The diagnostic as well as the refusal. A compile that failed for an unrelated
# reason - a stub that stopped parsing, an include that moved - refuses the call
# here too and would read as this test passing.
if ! grep -Eq 'too few arguments|no matching function|candidate expects' "$OMITTED_LOG"; then
  echo "ERROR: the compiler refused tests/key-policy-signature.cpp with the policy left" >&2
  echo "       out, and said nothing about the argument count. Its output is above. This" >&2
  echo "       test passes on a compile that failed for its own reason, so read what went" >&2
  echo "       wrong before trusting the refusal." >&2
  exit 1
fi

echo "PASS: a card registers with a key policy or it does not compile"
