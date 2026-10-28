#!/usr/bin/env bash
# Checks one compiled sketch image against the size of the partition it is
# flashed into, reading the partition table that build actually used.
#
#   usage: check-partition-fit.sh <binary> <partition-name|bytes:N> <label>
#
#   check-partition-fit.sh build/esp32.esp32.esp32/CAL.ino.bin   factory "CAL (factory)"
#   check-partition-fit.sh App/build/esp32.esp32.esp32/App.ino.bin ota_0 "App (ota_0)"
#   check-partition-fit.sh build/esp32.esp32.esp32/CAL.ino.bin   bytes:1441792 "CAL (field factory)"
#
# THE TABLE COMES FROM THE BINARY'S OWN BUILD DIRECTORY. arduino-cli copies the
# partitions.csv it compiled against into the build output directory beside the
# .bin, so `dirname <binary>`/partitions.csv is the layout this image was built
# for - whatever that file was named in the sketch root, and whatever is sitting
# in the sketch root now. That matters for two reasons. A board script selects a
# board by copying its table over partitions.csv for the length of one compile
# and restoring the other board's afterwards, so a check that read the sketch
# root after a build would size an 8MB image against a 4MB table. And a check
# that named one filename would cover only the board whose table happens to
# carry that name, leaving any other board's image compared against nothing.
#
# Each board's build directory is named after its FQBN, so the two boards' .bin
# files and the two tables beside them never overwrite each other, and a binary
# always resolves to the table of the build that produced it.
#
# WHY arduino-cli's OWN CEILING IS NOT THIS. Its "Sketch uses X of Y bytes" line
# measures against the PartitionScheme named in the FQBN rather than the table in
# the sketch: min_spiffs reports 1,966,080, and PartitionScheme=custom - which is
# what selects a sketch-supplied table on the S3 boards - raises Y to 16,777,216
# and so accepts anything either board can currently produce. The number that
# decides whether an image can be flashed is the size of its own partition.
#
# A missing binary, a missing table, a partition name absent from the table and
# an unreadable size are all failures. A skipped check and a passing check read
# the same way in a build log, so there is no path here that skips.
set -euo pipefail

BIN_PATH="${1:-}"
CEILING_SPEC="${2:-}"
LABEL="${3:-}"

if [ -z "$BIN_PATH" ] || [ -z "$CEILING_SPEC" ] || [ -z "$LABEL" ]; then
  echo "usage: check-partition-fit.sh <binary> <partition-name|bytes:N> <label>" >&2
  exit 2
fi

if [ ! -f "$BIN_PATH" ]; then
  echo "ERROR: ${LABEL}: no compiled image at ${BIN_PATH}." >&2
  echo "       Nothing was sized against anything. Build first, then check." >&2
  exit 1
fi

partition_size() {
  # $1: table path. $2: partition name from the table's own Name column.
  # Column layout: Name, Type, SubType, Offset, Size, Notes - Size is the 5th
  # comma-separated field, a hex literal like "0x1A0000". Prints bytes.
  local table="$1" name="$2" hex
  hex=$(grep -E "^[[:space:]]*${name}[[:space:]]*," "$table" \
        | head -1 | awk -F',' '{gsub(/[[:space:]]/,"",$5); print $5}')
  if [ -z "$hex" ]; then
    echo "ERROR: partition '${name}' is not in ${table}." >&2
    echo "       Partitions in that table: $(awk -F',' '!/^[[:space:]]*#/ && NF>4 {gsub(/[[:space:]]/,"",$1); if ($1 != "") printf "%s ", $1}' "$table")" >&2
    return 1
  fi
  if ! printf '%s' "$hex" | grep -qE '^(0[xX][0-9a-fA-F]+|[0-9]+)$'; then
    echo "ERROR: partition '${name}' in ${table} has an unreadable size '${hex}'." >&2
    return 1
  fi
  printf '%d' "$hex"
}

case "$CEILING_SPEC" in
  bytes:*)
    # An explicit ceiling, for a limit that is not a partition in any table in
    # this repository - the deployed fleet's pre-re-table factory size is the
    # one caller of this form.
    CEILING="${CEILING_SPEC#bytes:}"
    CEILING_SOURCE="the command line"
    if ! printf '%s' "$CEILING" | grep -qE '^[0-9]+$'; then
      echo "ERROR: ${LABEL}: '${CEILING_SPEC}' is not a byte count." >&2
      exit 1
    fi
    ;;
  *)
    TABLE="$(dirname "$BIN_PATH")/partitions.csv"
    if [ ! -f "$TABLE" ]; then
      echo "ERROR: ${LABEL}: no partition table at ${TABLE}." >&2
      echo "       arduino-cli writes the table it compiled against into the build output" >&2
      echo "       directory beside the image. Without it there is no ceiling to measure" >&2
      echo "       this image against, so this fails rather than passing unchecked." >&2
      exit 1
    fi
    CEILING=$(partition_size "$TABLE" "$CEILING_SPEC")
    CEILING_SOURCE="${TABLE}, the table this build compiled against"
    ;;
esac

if [ "$CEILING" -le 0 ]; then
  echo "ERROR: ${LABEL}: ceiling from ${CEILING_SOURCE} is ${CEILING} bytes." >&2
  exit 1
fi

ACTUAL=$(stat -c%s "$BIN_PATH")
PCT=$((ACTUAL * 100 / CEILING))
echo "    ${LABEL}: ${ACTUAL} / ${CEILING} bytes (${PCT}%), ${CEILING_SPEC} from ${CEILING_SOURCE}"

if [ "$ACTUAL" -gt "$CEILING" ]; then
  echo "ERROR: ${LABEL} image (${ACTUAL} bytes) exceeds ${CEILING_SPEC} (${CEILING} bytes)." >&2
  echo "       The ceiling came from ${CEILING_SOURCE}." >&2
  echo "       The size is the flashable image on disk, not arduino-cli's summary line." >&2
  echo "       An image this size does not fit and must not ship." >&2
  exit 1
fi
