#!/usr/bin/env bash
# Builds and runs the host test over BrandSplash.h: the length both ends of the
# brand-splash path test a body against.
#
# Once rather than once per board, the way ci/test-loader-order.sh runs: the
# asset is 240x120 on every panel CAL drives and nothing in BrandSplash.h is
# behind a target macro.
#
# BrandSplash.h includes only <stddef.h>, so this needs no stub directory.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
TEST_BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_BUILD_DIR"' EXIT
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror \
  -I. \
  tests/brand-splash-test.cpp -o "$TEST_BUILD_DIR/brand-splash-test"
echo "==> Brand splash length"
# The binary's own Passed line is what says it ran, rather than its exit
# status. A main() that returned without reaching an assertion exits 0 as
# happily as one that passed every case, and a gate reading the status cannot
# tell them apart. The line carries its own check count and its own failure
# count. See the same lines in ci/test-config-request.sh.
OUTPUT=$("$TEST_BUILD_DIR/brand-splash-test")
echo "$OUTPUT"
case "$OUTPUT" in
  *"Passed! "*" checks, 0 failed"*) ;;
  *)
    echo "ERROR: the brand splash test exited 0 without printing its Passed line." >&2
    exit 1
    ;;
esac
