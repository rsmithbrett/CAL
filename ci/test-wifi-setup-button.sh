#!/usr/bin/env bash
# Builds and runs the host test over the WiFi setup confirmation's decision:
# what a tap, an index and a timeout mean. WIFI_SETUP_BUTTON_DESIGN.md.
#
# WHY ONLY THE DECISION. App/WifiSetupButton.cpp draws through App/Display.h,
# which names LovyanGFX, so nothing under tests/ can link it - the same wall
# ci/test-personal-photos-key.sh records against CardManager::noteShowing().
# WifiSetupButton::answerFor() is constexpr in the header for that reason, and
# the .cpp's confirmation loop calls it rather than repeating the rule, so the
# loop and this test are reading one definition instead of two copies.
#
# What it cannot check: that the button is drawn where a thumb goes, that the
# label reads at arm's length on a wall, or that a tap reaches Touch at all.
# Those need the panel, and the panel needs a bench.
#
# THE PROPERTY THIS EXISTS FOR is that a timeout is a Cancel and never a
# Confirm. A display attempting WiFi is unattended by definition. The accident
# that argues for it is in CAL_TOUCH_RECOVERY_MENU.md: Set up Wi-Fi was the one
# menu row with no confirmation, one stray contact restarted a device into its
# portal, and the unit sat broadcasting an open setup AP until the abandonment
# timeout.
#
# Proven to bite before it was trusted, on 2026-11-05: making a timeout confirm
# on index 0 failed with "FAIL: a timeout is a Cancel" and exit 1. The mutation
# was gated on cmp and the header restored byte-identical afterwards.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

echo "==> The WiFi setup confirmation's decision"

TEST_BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_BUILD_DIR"' EXIT

# -Itests/key-policy-stubs for its Arduino.h, which is all the header needs: the
# decision is constexpr over a bool, a uint8_t and a bool, and names nothing
# else.
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror \
  -Itests/key-policy-stubs -IApp \
  tests/wifi-setup-button-test.cpp \
  -o "$TEST_BUILD_DIR/wifi-setup-button-test"

OUTPUT=$("$TEST_BUILD_DIR/wifi-setup-button-test")
echo "$OUTPUT"

# READ THE OUTPUT, NOT THE EXIT CODE. ci/test-brand-splash.sh, test-loader-order
# and test-recovery-menu each ran a binary and asked only whether it returned 0,
# so a main() that printed nothing passed all three - fixed on 2026-11-03 and
# not repeated here.
if ! grep -q '^PASS: wifi setup confirmation' <<<"$OUTPUT"; then
  echo "ERROR: the test ran but did not print its PASS line." >&2
  exit 1
fi
