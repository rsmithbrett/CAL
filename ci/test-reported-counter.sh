#!/usr/bin/env bash
# Builds and runs the host test over App/ReportedCounter.h's rule for counting
# occurrences into an 8-bit field that travels to the server.
#
# Host-side rather than on the device, for the reason ci/test-ram-picture-budget.sh
# gives: the rule is arithmetic over a figure the caller holds, and the range
# that matters is the whole of a uint8_t plus the step past its end. A device
# samples one value at a time, and reaching 256 decode failures on one takes a
# card failing all day.
#
# Once rather than once per board: nothing in the header is behind a target
# macro and no figure in it differs between the two builds.
#
# What this cannot check is that App/Assets.cpp's own increments go through
# countOne(). That is a call-site property and ci/check-asset-counters.sh is
# what reads it.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
TEST_BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_BUILD_DIR"' EXIT
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror \
  -IApp \
  tests/reported-counter-test.cpp -o "$TEST_BUILD_DIR/reported-counter-test"
echo "==> Reported counters against the end of a uint8_t"
# The binary's own PASS line is what says it ran, rather than its exit status. A
# main() that returned without reaching an assertion exits 0 as happily as one
# that passed every case, and a gate that reads the status cannot tell them
# apart. See the same lines in ci/test-config-request.sh.
OUTPUT=$("$TEST_BUILD_DIR/reported-counter-test")
echo "$OUTPUT"
case "$OUTPUT" in
  "PASS: reported counter"*) ;;
  *)
    echo "ERROR: the reported-counter test exited 0 without printing its PASS line." >&2
    exit 1
    ;;
esac
