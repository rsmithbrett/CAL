#!/usr/bin/env bash
# Builds and runs the host test over App/DecodeFailureReport.h - the accounting
# behind assetDecodeFailureCount/Ids and the three fields that say why a picture
# would not draw.
#
# The header names String and nothing else, which is why this compiles at all.
# Assets.cpp around it needs SD, mbedTLS, LovyanGFX and four other modules, so
# the accounting was moved out to where it could be run rather than stubbed in
# place - the same move ci/test-card-columns.sh describes.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
TEST_BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_BUILD_DIR"' EXIT
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror \
  -IApp -Itests/decode-failure-stubs \
  tests/decode-failure-report-test.cpp -o "$TEST_BUILD_DIR/decode-failure-report-test"
echo "==> Decode failure report accounting"
# The binary's own PASS line is what says it ran, rather than its exit status.
# See the same lines in ci/test-card-columns.sh.
#
# Captured with `|| true` and printed before it is judged, deliberately. Under
# `set -e` a command substitution that exits non-zero kills the script at that
# line, so the one run that most needs reading - the failing one - prints the
# names of the checks that failed to nowhere and the log shows only the heading.
OUTPUT=$("$TEST_BUILD_DIR/decode-failure-report-test" || true)
echo "$OUTPUT"
case "$OUTPUT" in
  "PASS: decode failure report"*) ;;
  *)
    echo "ERROR: the decode failure report test did not print its PASS line." >&2
    exit 1
    ;;
esac
