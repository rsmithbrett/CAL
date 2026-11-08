#!/usr/bin/env bash
# Builds and runs the host test over App/CardPolicyStore.h - the format a card
# policy is kept in across a restart, and the two decisions around it.
#
# Host-side rather than on the device, for the reason ci/test-unreachable-holdoff.sh
# gives: the cases that matter are a codec round trip and two predicates over
# figures the caller holds. Reproducing the refusals on a device means writing a
# deliberately corrupt NVS blob and rebooting for each one.
#
# Once rather than once per board: nothing in the header is behind a target macro,
# and the format is the same bytes on both - it is written and read by the same
# binary on the same device, so there is no cross-board compatibility question to
# size differently.
#
# What this cannot check is the NVS half in App/CardPolicyStore.cpp - that
# Preferences actually holds the blob across a power cut, that the restore runs
# before the first check-in, or that an over-budget policy really clears the keys.
# Those are call-site and flash properties; TEST_PLAN.md section 1g says what a
# bench run is still for.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
TEST_BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_BUILD_DIR"' EXIT
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror \
  -IApp \
  tests/card-policy-store-test.cpp -o "$TEST_BUILD_DIR/card-policy-store-test"
echo "==> The card policy a restart does not lose"
# The binary's own PASS line is what says it ran, rather than its exit status. A
# main() that returned without reaching an assertion exits 0 as happily as one
# that passed every case, and a gate that reads the status cannot tell them
# apart. See the same lines in ci/test-unreachable-holdoff.sh.
#
# Captured with `set -e` off and the status ignored, so a run that fails PRINTS
# its Failed: lines. Under `set -e` the assignment itself ends the script and the
# echo below never happens, which leaves a gate that says only "exit 1" about a
# test whose whole output is the reason. ci/test-unreachable-holdoff.sh and the
# scripts beside it still read that way.
set +e
OUTPUT=$("$TEST_BUILD_DIR/card-policy-store-test" 2>&1)
set -e
echo "$OUTPUT"
case "$OUTPUT" in
  "PASS: card policy store"*) ;;
  *)
    echo "ERROR: the card policy store test exited 0 without printing its PASS line." >&2
    exit 1
    ;;
esac
