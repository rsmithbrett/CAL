#!/usr/bin/env bash
# Builds and runs the host test over App/Loader.cpp's NVS write order.
#
# Once rather than once per board, the way ci/test-recovery-menu.sh runs: the
# order of two NVS commits is the same on both, and nothing in Loader.cpp is
# behind a target macro.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
TEST_BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_BUILD_DIR"' EXIT
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror \
  -Itests/loader-stubs -IApp \
  App/Loader.cpp tests/loader-order-test.cpp -o "$TEST_BUILD_DIR/loader-order-test"
echo "==> Loader NVS write order"
"$TEST_BUILD_DIR/loader-order-test"
