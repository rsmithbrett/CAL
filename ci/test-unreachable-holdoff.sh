#!/usr/bin/env bash
# Builds and runs the host test over App/UnreachableHoldOff.h's rule for whether
# a run of failed check-ins points at this device or at the far end.
#
# Host-side rather than on the device, for the reason ci/test-reported-counter.sh
# gives: the rule is a decision over figures the caller holds, and the cases that
# matter are a clock and a counter crossing their thresholds in both directions.
# Reproducing that on a device means an hour per case and a server you can take
# down.
#
# Once rather than once per board: nothing in the header is behind a target macro
# and no figure in it differs between the two builds.
#
# What this cannot check is that App.ino actually consults the header, or that
# gUnreachableSinceMs is stamped and cleared where it should be. Those are call-
# site properties; TEST_PLAN.md says what reads them.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
TEST_BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_BUILD_DIR"' EXIT
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror \
  -IApp \
  tests/unreachable-holdoff-test.cpp -o "$TEST_BUILD_DIR/unreachable-holdoff-test"
echo "==> The unreachable watchdog's hold-off decision"
# The binary's own PASS line is what says it ran, rather than its exit status. A
# main() that returned without reaching an assertion exits 0 as happily as one
# that passed every case, and a gate that reads the status cannot tell them
# apart. See the same lines in ci/test-config-request.sh.
OUTPUT=$("$TEST_BUILD_DIR/unreachable-holdoff-test")
echo "$OUTPUT"
case "$OUTPUT" in
  "PASS: unreachable hold-off"*) ;;
  *)
    echo "ERROR: the unreachable hold-off test exited 0 without printing its PASS line." >&2
    exit 1
    ;;
esac
