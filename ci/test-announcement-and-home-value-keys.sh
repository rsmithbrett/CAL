#!/usr/bin/env bash
# Builds and runs the host test over the content keys App/HomeValue.cpp and
# App/Announcement.cpp report: the address a valuation was computed for, the
# digest of a notice's text, and the conditions that decide whether either
# reports a key at all.
#
# Once rather than once per board, the way ci/test-personal-photos-key.sh runs:
# nothing in either module is behind a target macro, and neither reaches the
# network or the panel for anything this test drives.
#
# App/Cards.h, App/Display.h, App/Log.h, App/Announcement.h and App/HomeValue.h
# are compiled as they ship. All five are declarations and all five include
# <Arduino.h>, which tests/announcement-and-home-value-stubs provides; the
# functions the two modules call are defined in the test.
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
  -Itests/announcement-and-home-value-stubs -IApp \
  App/HomeValue.cpp App/Announcement.cpp tests/announcement-and-home-value-key-test.cpp \
  -o "$TEST_BUILD_DIR/announcement-and-home-value-key-test"
echo "==> Announcement and home value content keys"
# The binary's own PASS line is what says it ran, rather than its exit status. A
# main() that returned without reaching an assertion exits 0 as happily as one
# that passed every case, and a gate reading the status cannot tell them apart.
OUTPUT=$("$TEST_BUILD_DIR/announcement-and-home-value-key-test")
echo "$OUTPUT"
case "$OUTPUT" in
  *"PASS: announcement and home value content keys"*) ;;
  *)
    echo "ERROR: the content-key test exited 0 without printing its PASS line." >&2
    exit 1
    ;;
esac
