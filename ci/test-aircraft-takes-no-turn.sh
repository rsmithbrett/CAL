#!/usr/bin/env bash
# Builds and runs the host test over the aircraft card's place in the rotation:
# the card takes a turn while it holds a plane and at no other time, and no
# screen is drawn to say there is no plane.
#
# App/Aircraft.cpp and App/CardManager.cpp are compiled as they ship and linked
# together, so the card's fetch, item count and draw run under the scheduler's
# own poll(), history and policy code. tests/aircraft-stubs stands in for
# Arduino.h, ArduinoJson.h, HTTPClient.h, NetworkClientSecure.h and sdkconfig.h,
# and the test defines every function the two files call outside themselves.
#
# Once rather than once per board. tests/aircraft-stubs/sdkconfig.h leaves
# CONFIG_IDF_TARGET_ESP32S3 undefined, so App/CardManager.cpp takes the fleet
# board's branch of its one board gate, the action-button pin, which no case
# here presses.
#
# What this cannot check is in the test's own header: pixels, heap, TLS, either
# board's build, and how long a real fetch takes.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

# The stub accepts fetchMine()'s filter and does not apply it, so every field in
# a test body reaches the document whether or not the filter names it. A key
# missing from the real filter is dropped during deserialization and the parse
# takes its fallback forever, which no case here can see. These greps are what
# stands in for that: the three keys the countdown, the age line and the fetch
# schedule are read from have to be in the filter.
for KEY in nextFetchSeconds positionAgeSeconds showForSeconds; do
  if ! grep -q "\[\"$KEY\"\] = true;" App/Aircraft.cpp; then
    echo "ERROR: fetchMine()'s filter in App/Aircraft.cpp does not whitelist $KEY." >&2
    echo "       An un-whitelisted key never reaches the parse, and the host test" >&2
    echo "       cannot see that because its JSON stub does not apply the filter." >&2
    exit 1
  fi
done

TEST_BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_BUILD_DIR"' EXIT
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror \
  -Itests/aircraft-stubs -IApp \
  App/Aircraft.cpp App/CardManager.cpp tests/aircraft-takes-no-turn-test.cpp \
  -o "$TEST_BUILD_DIR/aircraft-takes-no-turn-test"
echo "==> The aircraft card takes no turn without a plane"
# The binary's own PASS line is what says it ran, rather than its exit status.
# Captured with `set -e` off so a run that fails prints its Failed: lines, the
# way ci/test-card-policy-store.sh does.
set +e
OUTPUT=$("$TEST_BUILD_DIR/aircraft-takes-no-turn-test" 2>&1)
set -e
echo "$OUTPUT"
case "$OUTPUT" in
  "PASS: aircraft card takes no turn without a plane"*) ;;
  *)
    echo "ERROR: the aircraft rotation test did not print its PASS line." >&2
    exit 1
    ;;
esac
