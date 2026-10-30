#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
TEST_BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_BUILD_DIR"' EXIT
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror \
  -Itests/recovery-stubs -I. Recovery.cpp tests/recovery-menu-test.cpp \
  -o "$TEST_BUILD_DIR/recovery-menu-test"
"$TEST_BUILD_DIR/recovery-menu-test"
