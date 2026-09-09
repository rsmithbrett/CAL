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
