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
  # The binary's own PASS line is what says it ran, rather than its exit
  # status. A main() that returned without reaching an assertion exits 0 as
  # happily as one that passed every case, and a gate reading the status cannot
  # tell them apart. Each board's run is read, so a build that prints the line
  # at one target and not the other is named. See the same lines in
  # ci/test-config-request.sh.
  OUTPUT=$("$TEST_BUILD_DIR/recovery-menu-test")
  echo "$OUTPUT"
  case "$OUTPUT" in
    *"PASS: recovery gesture"*) ;;
    *)
      echo "ERROR: the recovery menu test exited 0 at $BOARD without printing its PASS line." >&2
      exit 1
      ;;
  esac
done
