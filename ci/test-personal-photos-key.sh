#!/usr/bin/env bash
# Builds and runs the host test over App/PersonalPhotos.cpp's content key: the
# asset id a draw of the household's photographs reports, and the two conditions
# that decide whether it reports one.
#
# Once rather than once per board, the way ci/test-config-request.sh runs:
# nothing in PersonalPhotos.cpp is behind a target macro, and the difference
# between the two boards that reaches this card is whether an SD card mounted,
# which the test drives both ways through the Assets stubs.
#
# App/Assets.h, App/Cards.h, App/Display.h, App/Log.h and App/PersonalPhotos.h
# are compiled as they ship. All five are declarations and all five include
# <Arduino.h>, which tests/personal-photos-stubs provides; the functions
# PersonalPhotos.cpp calls are defined in the test.
#
# What this cannot check is CardManager::noteShowing() still asking for the
# resolver. That function names a graphics library through Display.h's drawing
# path and does not compile on a host, so the fallback order it applies is held
# by ci/check-content-key-resolvers.sh instead.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
TEST_BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_BUILD_DIR"' EXIT
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror \
  -Itests/personal-photos-stubs -IApp \
  App/PersonalPhotos.cpp tests/personal-photos-key-test.cpp \
  -o "$TEST_BUILD_DIR/personal-photos-key-test"
echo "==> Personal photographs content key"
# The binary's own PASS line is what says it ran, rather than its exit status. A
# main() that returned without reaching an assertion exits 0 as happily as one
# that passed every case, and a gate reading the status cannot tell them apart.
OUTPUT=$("$TEST_BUILD_DIR/personal-photos-key-test")
echo "$OUTPUT"
case "$OUTPUT" in
  *"PASS: personal photos content key"*) ;;
  *)
    echo "ERROR: the content-key test exited 0 without printing its PASS line." >&2
    exit 1
    ;;
esac
