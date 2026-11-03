#!/usr/bin/env bash
# Builds and runs the host test over App/RamPictureBudget.h's rule for whether a
# picture may be held in RAM beside a TLS session.
#
# Host-side rather than on the device, for the reason ci/test-card-columns.sh
# gives: the rule is pure arithmetic over figures the caller reads, and the
# range that matters spans four devices and two boards with different heap
# pools. A device can only ever sample its own numbers.
#
# What this cannot check is that the figures passed in on the device are the
# right ones - that HeapPools::largestTlsBlockBytes() rather than the bare
# 8-bit figure reaches fitsBesideTls(), and that the allocation really is
# cap-restricted to PSRAM when placedInExternalRam is true. Those are call-site
# properties and they are named in TEST_PLAN.md as unverified on hardware.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
TEST_BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_BUILD_DIR"' EXIT
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror \
  -IApp \
  tests/ram-picture-budget-test.cpp -o "$TEST_BUILD_DIR/ram-picture-budget-test"
echo "==> RAM picture budget against the TLS pool"
# The binary's own PASS line is what says it ran, rather than its exit status.
# See the same lines in ci/test-config-request.sh.
OUTPUT=$("$TEST_BUILD_DIR/ram-picture-budget-test")
echo "$OUTPUT"
case "$OUTPUT" in
  "PASS: ram picture budget"*) ;;
  *)
    echo "ERROR: the RAM picture budget test exited 0 without printing its PASS line." >&2
    exit 1
    ;;
esac
