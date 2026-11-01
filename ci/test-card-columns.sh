#!/usr/bin/env bash
# Builds and runs the host test over App/CardColumns.h's scoreboard column
# arithmetic.
#
# Once rather than once per board. The header's own static_asserts cover the two
# shipped panel widths and run in every arduino-cli build of both; what runs
# here is the range the assertions cannot walk, and the string measurements they
# have no way to read. The header names no graphics library, which is why this
# compiles at all - see the remarks at the top of it.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
TEST_BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_BUILD_DIR"' EXIT
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror \
  -IApp \
  tests/card-columns-test.cpp -o "$TEST_BUILD_DIR/card-columns-test"
echo "==> Scoreboard column arithmetic"
"$TEST_BUILD_DIR/card-columns-test"
