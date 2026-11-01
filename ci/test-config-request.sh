#!/usr/bin/env bash
# Builds and runs the host test over App/ConfigRequest.cpp's corner-tap gesture.
#
# Once rather than once per board, the way ci/test-loader-order.sh runs: nothing
# in ConfigRequest.cpp is behind a target macro, and the one figure that does
# differ between the two boards - the panel width - reaches it through
# Display::screenWidth(), which the test drives at both.
#
# App/Display.h and App/Log.h are compiled as they ship. Both are declarations
# and both include <Arduino.h>, which tests/config-request-stubs provides; the
# three functions ConfigRequest.cpp calls are defined in the test.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
TEST_BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_BUILD_DIR"' EXIT
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror \
  -Itests/config-request-stubs -IApp -Ilibraries/DamBoards/src \
  App/ConfigRequest.cpp tests/config-request-test.cpp \
  -o "$TEST_BUILD_DIR/config-request-test"
echo "==> App configuration-menu gesture"
# The binary's own PASS line is what says it ran, rather than its exit status. A
# main() that returned without reaching an assertion exits 0 as happily as one
# that passed every case, and a gate that reads the status cannot tell them
# apart.
OUTPUT=$("$TEST_BUILD_DIR/config-request-test")
echo "$OUTPUT"
case "$OUTPUT" in
  "PASS: config-request gesture"*) ;;
  *)
    echo "ERROR: the gesture test exited 0 without printing its PASS line." >&2
    exit 1
    ;;
esac
