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
"$TEST_BUILD_DIR/brand-splash-test"
