#!/usr/bin/env bash
# Builds and runs the host test over App/PersonalPhotosPresence.h's rule for what
# a check-in's `personalPhotoAssetIds` means: an absent member leaves the held
# photographs alone, an empty array clears them.
#
# Host-side rather than on the device, for the reason ci/test-unreachable-holdoff.sh
# gives: the rule is a decision over what the parser found, and the case that
# matters is a server whose photograph query threw - which on a device means
# taking a database down for one check-in and watching a card that is meant not
# to change.
#
# Once rather than once per board: nothing in the header is behind a target macro
# and no figure in it differs between the two builds.
#
# What this cannot check is that parsePersonalPhotos() consults the header at all,
# or that `is<JsonArrayConst>()` is what it asks the member with. No host test in
# this repository compiles App/CheckIn.cpp or includes ArduinoJson, so those are
# call-site properties; TEST_PLAN.md section 1h says what reads them.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
TEST_BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_BUILD_DIR"' EXIT
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror \
  -IApp \
  tests/personal-photos-absence-test.cpp -o "$TEST_BUILD_DIR/personal-photos-absence-test"
echo "==> What a check-in's personalPhotoAssetIds means"
# The binary's own PASS line is what says it ran, rather than its exit status. A
# main() that returned without reaching an assertion exits 0 as happily as one
# that passed every case, and a gate that reads the status cannot tell them
# apart. See the same lines in ci/test-config-request.sh.
OUTPUT=$("$TEST_BUILD_DIR/personal-photos-absence-test")
echo "$OUTPUT"
case "$OUTPUT" in
  "PASS: personal photographs presence"*) ;;
  *)
    echo "ERROR: the personal photographs presence test exited 0 without printing its PASS line." >&2
    exit 1
    ;;
esac
