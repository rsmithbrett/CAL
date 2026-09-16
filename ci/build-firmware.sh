#!/usr/bin/env bash
# Builds CAL and App and leaves flashable artifacts + checksums in
# build/esp32.esp32.esp32/ and App/build/esp32.esp32.esp32/ respectively.
# This is the entire build procedure, deliberately kept out of any CI
# provider's own config file: a GitHub Actions YAML, a GitLab pipeline, a
# Jenkinsfile, or a person's own terminal should all be able to call this
# one script and get the identical result. Only "how to get arduino-cli
# onto this machine" and "what to do with the artifacts afterward" (upload
# to a release, copy somewhere, etc.) are the CI provider's own business -
# everything about actually building the firmware lives here so switching
# CI providers never means re-deriving these steps.
#
# Both sketches share one toolchain install (same ESP32 core, same
# ArduinoJson/LovyanGFX versions - App has no library dependency CAL
# doesn't already have, confirmed by reading every #include across App/),
# so there is exactly one install pass below feeding two compile passes.
#
# Requires: arduino-cli already on PATH. Everything else it installs itself.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

# Pinned to match README.md's own toolchain table. An unpinned "latest" core
# or library would silently change the compiled size the partition table in
# partitions.csv was sized around - see that file's own comments on why
# CAL's factory partition has as little headroom as it does.
ESP32_CORE_VERSION="3.3.11"
LOVYANGFX_VERSION="1.2.28"
ARDUINOJSON_VERSION="7.4.3"
BOARD_INDEX_URL="https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json"
FQBN="esp32:esp32:esp32:PartitionScheme=min_spiffs"
BUILD_DIR="build/esp32.esp32.esp32"

echo "==> Installing esp32:esp32@${ESP32_CORE_VERSION}"
arduino-cli core update-index --additional-urls "$BOARD_INDEX_URL"
arduino-cli core install "esp32:esp32@${ESP32_CORE_VERSION}" --additional-urls "$BOARD_INDEX_URL"

# LovyanGFX and ArduinoJson only. The QR generator is vendored in-tree as
# CalQr.c/.h specifically so no qrcode library needs installing here - see
# Display.cpp's own comment on why a plain <qrcode.h> would resolve to the
# ESP32 core's unrelated esp_qrcode header instead of a library CAL depends on.
echo "==> Installing libraries"
arduino-cli lib install "LovyanGFX@${LOVYANGFX_VERSION}" "ArduinoJson@${ARDUINOJSON_VERSION}"

# No PartitionScheme value in the FQBN above actually matters: a
# partitions.csv sitting in the sketch root overrides whatever scheme is
# named on the command line. Verified by decoding the compiled output
# binary's own partition table (magic 0xAA50 entries) rather than trusted
# from the CLI's summary line, which reports against the FQBN's static
# memory map and not against the table actually baked into the binary.
# THE TWO PARTITION TABLES MUST BE THE SAME FILE'S CONTENTS.
#
# Each sketch takes the partitions.csv sitting beside it, so App/ has its own
# copy - and on 2026-09-15 that copy was found to be two revisions stale: it
# still had factory at 0x160000, ota_0 at 0x250000 (the size from before callog
# was carved out) and no callog partition at all.
#
# That is not cosmetic, because App.ino.merged.bin is a published release asset
# and bakes its table in at 0x8000. Flashing it would have silently reverted a
# device to a layout with no callog and an ota_0 running straight through where
# callog now lives - undoing a re-table that can only be done over USB, using an
# artifact whose whole purpose is to be flashable.
#
# Checked rather than copied here on purpose: copying would fix the build and
# leave the repository lying, so the next person reading App/partitions.csv would
# still see the wrong numbers.
echo "==> Checking the two partition tables agree"
if ! diff -q partitions.csv App/partitions.csv >/dev/null; then
  echo "ERROR: partitions.csv and App/partitions.csv differ." >&2
  echo "       Both sketches must be built against the same layout, and App.ino.merged.bin" >&2
  echo "       bakes its copy into the published image - a stale one silently re-tables any" >&2
  echo "       device it is flashed to. Make them identical and re-run." >&2
  diff partitions.csv App/partitions.csv >&2 || true
  exit 1
fi

# EVERY PUBLIC MOTION ENTRY POINT MUST HAVE A CALLER.
#
# Motion::noteTouchAndShouldSwallow() shipped in v2026.09.15.0004 declared,
# defined, documented, and called from nowhere at all. begin(), applyPolicy() and
# service() were wired into App.ino; the touch wake was not. The consequence was
# not a missing feature but an unrecoverable one: device 17 faded to 0% on its dim
# timeout and could not be woken by touching the glass, because nothing told the
# motion module a finger had arrived. Eight hours of black screen, ending in a
# power cycle - which is precisely what a deployed panel in somebody's lobby does
# not have available.
#
# A compiler cannot catch this. An uncalled non-static function in a module that
# is linked anyway is perfectly legal C++ and warns about nothing. The firmware
# has no unit-test harness to catch it either. So it is checked the only way it
# can be: the header declares the contract, and something outside the module has
# to use it.
#
# Deliberately a call-site count and nothing cleverer. It does not know whether
# the call is in the right place, or reached on the right branch - it knows the
# difference between "wired" and "not wired at all", which is the failure that
# actually happened and the one worth a gate.
echo "==> Checking every public Motion entry point is called from somewhere"
MOTION_UNWIRED=0
while read -r FN; do
  [ -z "$FN" ] && continue
  # Call sites anywhere in App/ other than Motion's own two files. grep -w so
  # state() does not match operatingState(), and the Motion.* exclusion so a
  # function calling itself, or its own declaration, never counts as a caller.
  CALLERS=$(grep -rl --include='*.cpp' --include='*.ino' -w "$FN" App/ 2>/dev/null \
            | grep -v 'App/Motion\.' | wc -l)
  if [ "$CALLERS" -eq 0 ]; then
    echo "ERROR: Motion::$FN() is declared in App/Motion.h but called from nowhere." >&2
    MOTION_UNWIRED=1
  fi
done <<'FNLIST'
begin
applyPolicy
service
noteTouchAndShouldSwallow
capabilityToReport
clearReportedCounters
FNLIST

if [ "$MOTION_UNWIRED" -ne 0 ]; then
  echo "       A motion entry point with no caller is a feature that silently does not run," >&2
  echo "       and in the touch-wake case it is a panel a household cannot recover. Wire it" >&2
  echo "       (see MOTION_AWARE_DISPLAY_DESIGN.md section 9) or remove it from the header." >&2
  exit 1
fi

# CAPABILITY MUST BE ON THE CHECK-IN REQUEST, NOT ONLY ON TELEMETRY.
#
# The caller count above proves a function is wired to something. It cannot prove it
# is wired to the RIGHT something, and on 2026-09-16 that gap cost a real defect:
# Motion::capabilityToReport() was called from Telemetry.cpp only, so the value
# landed on a diagnostic row while the server's policy gate - which reads
# Device.MotionCapability, written exclusively by CheckInGatewayService - stayed
# empty forever. A device could never retire an operator's declaration. It reported
# "motion 4 seconds ago" on telemetry while the server still did not know it had a
# sensor.
#
# So this is the design rule as a check rather than as a paragraph:
# MOTION_AWARE_DISPLAY_DESIGN.md section 2, "capability rides the check-in request,
# not telemetry". Telemetry may ALSO carry it - it is a useful diagnostic and it is
# how the contradiction became visible - but check-in must.
echo "==> Checking motion capability is sent on the check-in request"
if ! grep -q 'requestDoc\["motionCapability"\]' App/CheckIn.cpp 2>/dev/null; then
  echo "ERROR: App/CheckIn.cpp does not send motionCapability." >&2
  echo "       The server gates motion policy on Device.MotionCapability, which is only" >&2
  echo "       ever written from the check-in path (CheckInGatewayService). Reporting" >&2
  echo "       capability on telemetry alone writes it to a diagnostic row that feeds no" >&2
  echo "       decision, and a device can then never retire an operator's declaration." >&2
  echo "       See MOTION_AWARE_DISPLAY_DESIGN.md section 2." >&2
  exit 1
fi

echo "==> Compiling CAL"
arduino-cli compile --fqbn "$FQBN" --export-binaries .

echo "==> Compiling App"
(
  cd App
  arduino-cli compile --fqbn "$FQBN" --export-binaries .
)

# arduino-cli's own "Sketch uses X of Y bytes" line checks the FQBN's generic
# min_spiffs scheme (1,966,080 bytes), not the real, asymmetric partitions.csv
# actually baked into the binary - a build well within the true factory or
# ota_0 ceiling can look alarming there, and worse, a build that has genuinely
# grown past the real ceiling still reports "fits" against the wrong number.
# This reads partitions.csv itself (the one source of truth for both real
# ceilings) rather than trusting either arduino-cli's message or a
# hand-maintained constant here that could drift from partitions.csv the next
# time someone resizes a partition.
echo "==> Verifying compiled size against the real partition table"
partition_size() {
  # $1: partition name from partitions.csv's own Name column (e.g. "factory").
  # Column layout: Name, Type, SubType, Offset, Size, Notes - Size is the 5th
  # comma-separated field, a hex literal like "0x160000".
  local hex
  hex=$(grep -E "^${1}," partitions.csv | head -1 | awk -F',' '{gsub(/ /,"",$5); print $5}')
  printf '%d' "$hex"
}
check_size() {
  # $1: human label for the message. $2: compiled .bin path. $3: real
  # ceiling in bytes, from partition_size above.
  local label="$1" bin_path="$2" ceiling="$3"
  local actual
  actual=$(stat -c%s "$bin_path")
  local pct=$((actual * 100 / ceiling))
  echo "    ${label}: ${actual} / ${ceiling} bytes (${pct}%)"
  if [ "$actual" -gt "$ceiling" ]; then
    echo "ERROR: ${label} binary (${actual} bytes) exceeds its real partition ceiling (${ceiling} bytes)." >&2
    echo "        This is the actual flashable image size, not arduino-cli's generic scheme check above - a build this size will not fit and must not ship." >&2
    exit 1
  fi
}
check_size "CAL (factory)" "${BUILD_DIR}/CAL.ino.bin" "$(partition_size factory)"
check_size "App (ota_0)" "App/${BUILD_DIR}/App.ino.bin" "$(partition_size ota_0)"

# CAL MUST ALSO FIT ota_0, WHICH IS NOT WHERE IT RUNS.
#
# This is the one arithmetic requirement of the CAL-over-the-air design
# (CAL_OTA_DESIGN.md section 5.4). That design timeshares ota_0 rather than
# carving a staging partition out of it: CAL is downloaded into ota_0, booted
# there, and the candidate copies itself into factory. Nothing ever writes the
# partition it is executing from, and otadata always names a partition that is
# whole.
#
# Carving a staging partition was rejected on measurement, not preference. A CAL
# image needs 1,337,360 bytes; ota_0's slack after the App is 604,928 - short by
# 732,432. It is short by 421,888 against the ENTIRE 4MB with zero headroom
# anywhere, and still short by 94,208 after deleting callog, spiffs and coredump.
# There is no version of this table with room for a second app-sized partition.
#
# So the whole feature rests on CAL fitting a partition it was never sized for,
# and that invariant has no other guard. factory is 1,703,936 and ota_0 is
# 2,097,152, so today CAL fits both comfortably - but factory is the SMALLER of
# the two, which means a CAL that fits its own home could still be the one that
# breaks this if the table is ever rebalanced the other way. Checked here so the
# build says so rather than a device discovering it during an update.
check_size "CAL (must also fit ota_0, for OTA staging)" "${BUILD_DIR}/CAL.ino.bin" "$(partition_size ota_0)"

echo "==> Computing checksums"
(
  cd "$BUILD_DIR"
  sha256sum CAL.ino.merged.bin CAL.ino.bin CAL.ino.bootloader.bin CAL.ino.partitions.bin > checksums.txt
)
(
  cd "App/$BUILD_DIR"
  sha256sum App.ino.merged.bin App.ino.bin App.ino.bootloader.bin App.ino.partitions.bin > checksums.txt
)
# One combined file is what actually goes in a release's notes/body and is
# published as its own downloadable asset - a release with two separate
# checksums.txt files floating among its other assets would just mean
# guessing which one covers which binary.
cat "$BUILD_DIR/checksums.txt" "App/$BUILD_DIR/checksums.txt" | tee combined-checksums.txt

echo "==> Done. CAL artifacts in ${BUILD_DIR}/, App artifacts in App/${BUILD_DIR}/"
