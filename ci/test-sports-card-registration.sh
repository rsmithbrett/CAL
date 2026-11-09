#!/usr/bin/env bash
# Builds and runs the host test over App/Sports.cpp's registrations and the three
# hook switches that bind them: which of the five ids register at all, which of
# them take the table renderer, which card's games each registered hook answers
# with, and how many frames a showing is worth.
#
# Once rather than once per board, the way ci/test-announcement-and-home-value-keys.sh
# runs: nothing in Sports.cpp is behind a target macro, and nothing this test
# drives reaches the network or the panel - the renderer is stubbed and what the
# cases read is which card's rows arrived at it.
#
# App/Sports.h, App/Cards.h, App/CardColumns.h, App/Display.h and App/Log.h are
# compiled as they ship. All five are declarations, and all five include
# <Arduino.h>, which tests/sports-card-stubs provides; the nine functions
# Sports.cpp calls are defined in the test. CardColumns::boardScoreIsDrawable()
# needs no stub - it is constexpr in that header.
#
# WHY THIS EXISTS. Registering sportsscores2 changed three things inside
# Sports.cpp, and a mutation sweep found every one of them invisible to all 24
# runners in this directory: a second strcmp in isBoard(), a registerOne(4, ...)
# call, and a `case 3:` added to each of the three switches so their default arm
# could hand out index 4. The static_assert that commit also added is reached
# only by the arduino build, which runs after these gates.
#
# What this cannot check is that static_assert itself, which a compiler reaches
# and a test binary does not. The test holds its own copy of the id table to
# Sports::kMaxCards with a static_assert instead, so raising the cap without
# extending that table stops this test compiling, and its identity cases then
# require the new index to answer with its own games rather than the default
# arm's.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
TEST_BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_BUILD_DIR"' EXIT
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror \
  -Itests/sports-card-stubs -IApp \
  App/Sports.cpp tests/sports-card-registration-test.cpp \
  -o "$TEST_BUILD_DIR/sports-card-registration-test"
echo "==> Sports card registration and hook switches"
# The binary's own PASS line is what says it ran, rather than its exit status. A
# main() that returned without reaching an assertion exits 0 as happily as one
# that passed every case, and a gate reading the status cannot tell them apart.
OUTPUT=$("$TEST_BUILD_DIR/sports-card-registration-test")
echo "$OUTPUT"
case "$OUTPUT" in
  *"PASS: sports card registration and hook switches"*) ;;
  *)
    echo "ERROR: the sports registration test exited 0 without printing its PASS line." >&2
    exit 1
    ;;
esac
