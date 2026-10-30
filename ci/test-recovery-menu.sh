#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
TEST_BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_BUILD_DIR"' EXIT
for BOARD in esp32 esp32s3; do
  TARGET_FLAGS=()
  if [ "$BOARD" = esp32s3 ]; then TARGET_FLAGS=(-DCONFIG_IDF_TARGET_ESP32S3=1); fi
  "${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror "${TARGET_FLAGS[@]}" \
    -Itests/recovery-stubs -I. -Ilibraries/DamBoards/src \
    Recovery.cpp tests/recovery-menu-test.cpp -o "$TEST_BUILD_DIR/recovery-menu-test"
  echo "==> Recovery tests: $BOARD"
  "$TEST_BUILD_DIR/recovery-menu-test"
done
