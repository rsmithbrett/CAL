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
# Twice, with the second build bounds-checked. cardListingId() opens with
# `itemIndex >= gCount` and the condition below it answers empty for every index
# the module's own state can reach past the set, so the case about an item past
# the set reads the same with that guard taken out: what the guard holds is the
# read of gOnCard[item] and gIds[item], not the answer. -fsanitize=bounds
# instruments both arrays against their declared kMaxPhotos, and
# -fsanitize-undefined-trap-on-error makes a violation a trap instead of a
# diagnostic, so no sanitizer runtime has to be present and the binary stops
# before its PASS line. The test asks for kMaxPhotos itself, which is one past
# both arrays.
#
# What this cannot check is CardManager::noteShowing() still asking for the
# resolver. That function names a graphics library through Display.h's drawing
# path and does not compile on a host, so the fallback order it applies is held
# by ci/check-content-key-resolvers.sh instead.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
TEST_BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_BUILD_DIR"' EXIT

build() {
  "${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror \
    "${@:2}" \
    -Itests/personal-photos-stubs -IApp \
    App/PersonalPhotos.cpp tests/personal-photos-key-test.cpp \
    -o "$TEST_BUILD_DIR/$1"
}

# The binary's own PASS line is what says it ran, rather than its exit status. A
# main() that returned without reaching an assertion exits 0 as happily as one
# that passed every case, and a gate reading the status cannot tell them apart.
# A trapping bounds violation prints no PASS line either, which is how the
# second run reports one.
run() {
  local label="$1"
  local binary="$2"
  local output
  echo "==> $label"
  output=$("$TEST_BUILD_DIR/$binary" || true)
  echo "$output"
  case "$output" in
    *"PASS: personal photos content key"*) ;;
    *)
      echo "ERROR: $label finished without printing its PASS line." >&2
      exit 1
      ;;
  esac
}

build personal-photos-key-test
build personal-photos-key-test-bounds \
  -fsanitize=bounds -fsanitize-undefined-trap-on-error

run "Personal photographs content key" personal-photos-key-test
run "Personal photographs content key, array bounds checked" personal-photos-key-test-bounds
