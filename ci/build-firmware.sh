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

# THE SECOND BOARD. Same toolchain, same libraries, different chip, different
# partition table - see the Panellan block further down for what each of these
# FQBN options is for and why none of them is optional.
S3_FQBN="esp32:esp32:esp32s3:PSRAM=enabled,CDCOnBoot=cdc,PartitionScheme=custom"
S3_BUILD_DIR="build/esp32.esp32.esp32s3"
PANELLAN_TABLE="partitions-panellan-8mb.csv"

# The board's wire name, which is the suffix on its published asset names. It is
# DeviceBoard's own string on the server side and it is a WIRE CONTRACT: the
# server looks for an asset called exactly this and nothing tells either side
# when they disagree, because a release holding no such asset reads the same as a
# release published before this board existed. See README's "One release per
# version" table and the server's FirmwareReleaseAssets.
PANELLAN_WIRE="panellan-zx2d80ce02s"

# BoardPanellan.h lives in an in-repo library rather than twice in two sketch
# folders, so both sketches read one pin map. Passed to every compile including
# the 4MB ones: their include of it is behind CONFIG_IDF_TARGET_ESP32S3 so they
# do not need it, and passing it anyway means one compile line shape rather than
# two. The flag ADDS to the default search path, so LovyanGFX and ArduinoJson
# still resolve the way they did.
LIBRARIES_DIR="libraries"

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

# AND THEY HAVE TO BE THE COMMITTED ONES, which the check above cannot tell.
#
# The Panellan pass further down swaps the 8MB table in under the name
# partitions.csv and restores it. A run that died between those two steps leaves
# BOTH copies holding the 8MB table - so they agree with each other, the gate
# above passes, and the 4MB compiles that follow build against the other board's
# layout. Every table-derived ceiling in the first half of this log is then the
# wrong board's and reports green, and App.ino.merged.bin bakes an 8MB table in
# at 0x8000.
#
# This check used to sit beside the swap, which is after those compiles. It
# refused, so nothing shipped, but only once a full 4MB build had been measured
# against the wrong numbers. Two seconds here instead.
#
# git diff is given HEAD and a SINGLE trailing --, deliberately. Outside a
# repository `git diff --quiet -- a b` falls back to --no-index and compares the
# two paths AGAINST EACH OTHER, which returns 0 for two copies of the same wrong
# table - so the guard passed and the restore's own verification passed while an
# 8MB table sat under the 4MB name. Naming HEAD cannot fall into --no-index, and
# the work-tree test below says plainly when there is no git to ask.
require_committed_partition_tables() {
  if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "ERROR: this is not a git work tree, so the partition tables cannot be checked" >&2
    echo "       against HEAD - and this script swaps one of them in and out." >&2
    echo "       Run it from a git checkout. An exported or copied tree cannot tell a" >&2
    echo "       restored table from an 8MB one left behind by an interrupted run." >&2
    exit 1
  fi

  if ! git diff --quiet HEAD -- partitions.csv App/partitions.csv; then
    echo "ERROR: partitions.csv and/or App/partitions.csv differ from HEAD." >&2
    echo "       A previous run may have been interrupted between the 8MB swap and the" >&2
    echo "       restore. Inspect them, restore the 4MB table to both, then re-run." >&2
    git diff --stat HEAD -- partitions.csv App/partitions.csv >&2 || true
    exit 1
  fi
}

echo "==> Checking the partition tables are the committed ones"
require_committed_partition_tables

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
#
# The names come out of App/Motion.h rather than out of a list written here. A
# list written here is a list that stops growing: six names sat in one while the
# header declared twelve, so half the module was outside the gate and the
# thirteenth entry point would have landed outside it too. Reading the header
# makes the figure move on its own, and the floor below is what says the reading
# worked.
echo "==> Checking every public Motion entry point is called from somewhere"
motion_entry_points() {
  # Function declarations in App/Motion.h: a return type, a name, an argument
  # list, and ');' on the same line. Leading whitespace is allowed, so indenting
  # the namespace body changes the indentation and not the list. Struct fields and
  # enum members carry no argument list and match nothing here.
  grep -E '^[[:space:]]*[A-Za-z_][A-Za-z0-9_:<>&* ]*\**[A-Za-z0-9_]+\(.*\);[[:space:]]*$' App/Motion.h \
    | sed -E 's/^.*[^A-Za-z0-9_]([A-Za-z_][A-Za-z0-9_]*)\(.*$/\1/'
}
MOTION_ENTRY_POINTS=$(motion_entry_points || true)
MOTION_ENTRY_COUNT=$(printf '%s\n' "$MOTION_ENTRY_POINTS" | grep -c . || true)
echo "    $MOTION_ENTRY_COUNT entry points read out of App/Motion.h"

if [ "$MOTION_ENTRY_COUNT" -eq 0 ]; then
  echo "ERROR: no function declaration was found in App/Motion.h, so this gate read" >&2
  echo "       nothing at all. The pattern above stopped matching the header." >&2
  exit 1
fi
if [ "$MOTION_ENTRY_COUNT" -lt 12 ]; then
  echo "ERROR: only $MOTION_ENTRY_COUNT entry points were read out of App/Motion.h and there were" >&2
  echo "       12 on 2026-11-03. Either entry points were removed, or the pattern stopped" >&2
  echo "       matching and the loop below ran over less than the header declares." >&2
  exit 1
fi

# ENTRY POINTS A DESIGN DOCUMENT CLAIMS BEFORE ANY CODE CALLS THEM.
#
# "Called from nowhere in this repository" and "unused" are not the same reading,
# and the difference is a second repository. CAL's design documents live beside
# the server tree, so a grep run from here sees neither the document that names an
# entry point nor the feature waiting to call it.
#
# hasEverDetectedMotion() was read as dead on 2026-11-03 and very nearly removed.
# CARD_ATTENTION_DESIGN.md section 2, in the server tree, excludes a display that
# has never confirmed motion from every denominator and carries that fact into the
# day row as SensorConfirmed, naming this accessor as the one thing in the firmware
# that can support a claim of Present. Section 3 lists it again among the five
# Motion entry points that design reads. SensorConfirmed was not built because the
# day row was not built. Removing the accessor would leave that design re-deriving
# Present from GPIO35, which is the thing both documents forbid.
#
# So the exemption is a name plus the reason, and the reason is here because this
# is the file somebody reads when they want the name out. Two readings keep the
# list from outliving what it exempts: a name App/Motion.h no longer declares
# fails, and so does a name that has acquired a caller, because the exemption is
# then spent. Taking a name out while it still has no caller fails on the ordinary
# arm below, which is the point.
motion_exemptions() {
  cat <<'EXEMPT'
hasEverDetectedMotion|CARD_ATTENTION_DESIGN.md sections 2 and 3, server tree: SensorConfirmed on the day row, not built
EXEMPT
}
MOTION_EXEMPT_NAMES=$(motion_exemptions | cut -d'|' -f1)

for FN in $MOTION_EXEMPT_NAMES; do
  if ! printf '%s\n' "$MOTION_ENTRY_POINTS" | grep -qx "$FN"; then
    echo "ERROR: the exemption list names Motion::$FN(), which App/Motion.h no longer" >&2
    echo "       declares. It was exempt because:" >&2
    motion_exemptions | grep "^$FN|" | cut -d'|' -f2 | sed 's/^/       /' >&2
    echo "       Either the declaration came back or the exemption goes with it." >&2
    exit 1
  fi
done

# Every caller writes Motion::<name>(, which is the only spelling counted below.
# An unqualified call would need a using declaration to compile, and this is what
# keeps one from appearing and reading as no caller at all.
if grep -rqE 'using namespace Motion|using Motion::' --include='*.cpp' --include='*.ino' App/ 2>/dev/null; then
  echo "ERROR: something under App/ pulls Motion's names into its own scope." >&2
  grep -rnE 'using namespace Motion|using Motion::' --include='*.cpp' --include='*.ino' App/ >&2 || true
  echo "       The caller count below reads Motion::<name>( and nothing else, so a call" >&2
  echo "       made through a using declaration reads here as no caller. Call it as" >&2
  echo "       Motion::<name>()." >&2
  exit 1
fi

MOTION_UNWIRED=0
MOTION_WIRED=0
MOTION_UNWIRED_NAMES=
MOTION_EXEMPT_UNWIRED=0
MOTION_EXEMPT_SPENT=
while read -r FN; do
  [ -z "$FN" ] && continue
  # Files holding a Motion::<name>( call, anywhere in App/ other than Motion's own
  # two files, so a function calling itself, or its own declaration, never counts
  # as a caller. The Motion:: prefix is what keeps state() apart from
  # operatingState() and from a local variable of the same name. The '|| true'
  # keeps a grep that matches nothing from taking the script down through pipefail
  # before the arm below can name the function.
  CALLERS=$(grep -rlF --include='*.cpp' --include='*.ino' "Motion::$FN(" App/ 2>/dev/null \
            | grep -v 'App/Motion\.' | wc -l || true)
  if printf '%s\n' "$MOTION_EXEMPT_NAMES" | grep -qx "$FN"; then
    if [ "$CALLERS" -eq 0 ]; then
      MOTION_EXEMPT_UNWIRED=$((MOTION_EXEMPT_UNWIRED + 1))
    else
      MOTION_EXEMPT_SPENT="$MOTION_EXEMPT_SPENT $FN"
    fi
  elif [ "$CALLERS" -eq 0 ]; then
    MOTION_UNWIRED=1
    MOTION_UNWIRED_NAMES="$MOTION_UNWIRED_NAMES $FN"
  else
    MOTION_WIRED=$((MOTION_WIRED + 1))
  fi
done <<FNLIST
$MOTION_ENTRY_POINTS
FNLIST

echo "    $MOTION_WIRED of $MOTION_ENTRY_COUNT called from outside the module, $MOTION_EXEMPT_UNWIRED exempt and still uncalled"
motion_exemptions | sed 's/|/: /' | sed 's/^/    exempt: /'

if [ -n "$MOTION_EXEMPT_SPENT" ]; then
  for FN in $MOTION_EXEMPT_SPENT; do
    echo "ERROR: Motion::$FN() is on the exemption list and now has a caller, so the" >&2
    echo "       exemption is spent. It was exempt because:" >&2
    motion_exemptions | grep "^$FN|" | cut -d'|' -f2 | sed 's/^/       /' >&2
    echo "       Take the name off the list and let the ordinary count hold it." >&2
  done
  exit 1
fi

if [ "$MOTION_UNWIRED" -ne 0 ]; then
  for FN in $MOTION_UNWIRED_NAMES; do
    echo "ERROR: Motion::$FN() is declared in App/Motion.h but called from nowhere." >&2
  done
  echo "       A motion entry point with no caller is a feature that does not run and" >&2
  echo "       reports nothing, and in the touch-wake case it is a panel a household" >&2
  echo "       cannot recover. CAL's design documents are in the server tree rather than" >&2
  echo "       this repository - MOTION_AWARE_DISPLAY_DESIGN.md section 9 and" >&2
  echo "       CARD_ATTENTION_DESIGN.md are both there - so read them before calling this" >&2
  echo "       dead. Then wire it, take it out of the header, or put it on the exemption" >&2
  echo "       list above with the document and section that claims it." >&2
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

# THE TWO RestartCause ENUMS ARE A WIRE FORMAT, AND NO COMPILER CHECKS THEM.
#
# BootDiag.h and App/BootDiag.h each declare RestartCause. They are deliberately
# separate files - the two sketches compile independently and share no headers,
# which is the same reason Identity, Display and Tls exist twice here. CAL writes
# the numeric value into the "bootdiag" NVS namespace immediately before
# esp_restart(); the App reads it back on the boot that follows and reports it on
# every telemetry row from then on.
#
# So this is two binaries on the SAME device agreeing on a number, and nothing in
# either build can notice if they stop agreeing. The two sketches are never
# compiled together, so a value that means CAL_HANDOVER in one and
# CAL_INSTALLED_APP in the other is perfectly legal C++ on both sides. The device
# would then confidently report the wrong cause for exactly the restart that
# installed the drift - a diagnostic that lies is worse than one that is missing,
# because the missing one at least says so (see CAL_OTA_DESIGN.md section 13).
#
# Checked the same way the partition tables above are checked, and for the same
# reason: the invariant is real, the failure reports nothing, and a comment in
# each file asking the next person to be careful is not a mechanism.
#
# A text comparison because there is nowhere to put a compile-time one. The two
# enums carry the same fully qualified name, BootDiag::RestartCause, so no
# translation unit can see both and a static_assert has nothing to stand in.
# Generating both from one list would give the compiler the answer, and it would
# also undo the separation CAL_OTA_DESIGN.md section 13 asks for and throw away
# the per-value documentation each file keeps in its own words. A list of
# static_asserts would be a third hand-written census of the same names, which is
# the thing that went wrong here in the first place. So the comparison stays
# textual, and what it reads is counted and floored, because a pattern that stops
# matching returns no values and two empty readings compare equal.
echo "==> Checking the two RestartCause enums agree"
restart_causes() {
  # $1: path to a BootDiag.h. Emits "Name = N" per line, in declaration order,
  # from that file's enum body only. Leading whitespace comes off after the match
  # rather than being part of it, so re-indenting a header, or wrapping it in a
  # namespace, changes the indentation and not the list.
  sed -n '/enum class RestartCause/,/^[[:space:]]*};/p' "$1" \
    | grep -oE '^[[:space:]]*[A-Za-z][A-Za-z0-9_]* = [0-9]+' \
    | sed 's/^[[:space:]]*//'
}
restart_cause_members() {
  # Member lines in the same body, counted whether or not they carry a number, so
  # a value taking its number from its position is counted here and not above.
  sed -n '/enum class RestartCause/,/^[[:space:]]*};/p' "$1" \
    | grep -cE '^[[:space:]]*[A-Za-z][A-Za-z0-9_]*[[:space:]]*(=[^,]+)?,[[:space:]]*$'
}
CAL_CAUSES=$(restart_causes BootDiag.h || true)
APP_CAUSES=$(restart_causes App/BootDiag.h || true)
CAL_CAUSE_COUNT=$(printf '%s\n' "$CAL_CAUSES" | grep -c . || true)
APP_CAUSE_COUNT=$(printf '%s\n' "$APP_CAUSES" | grep -c . || true)
CAL_MEMBER_COUNT=$(restart_cause_members BootDiag.h || true)
APP_MEMBER_COUNT=$(restart_cause_members App/BootDiag.h || true)
echo "    $CAL_CAUSE_COUNT numbered values read from BootDiag.h, $APP_CAUSE_COUNT from App/BootDiag.h"

RESTART_CAUSE_FAIL=0
if [ "$CAL_CAUSE_COUNT" -eq 0 ] || [ "$APP_CAUSE_COUNT" -eq 0 ]; then
  echo "ERROR: BootDiag.h read as $CAL_CAUSE_COUNT RestartCause values and App/BootDiag.h as" >&2
  echo "       $APP_CAUSE_COUNT, so the comparison below had nothing to compare. The enum was" >&2
  echo "       renamed, moved, or laid out some way the pattern above no longer matches." >&2
  RESTART_CAUSE_FAIL=1
elif [ "$CAL_CAUSE_COUNT" -lt 12 ] || [ "$APP_CAUSE_COUNT" -lt 12 ]; then
  echo "ERROR: BootDiag.h gave $CAL_CAUSE_COUNT RestartCause values and App/BootDiag.h gave" >&2
  echo "       $APP_CAUSE_COUNT, and there were 12 in each on 2026-11-03. A value here never goes" >&2
  echo "       away - the server holds rows carrying these tokens back to September, and" >&2
  echo "       the six-month compatibility mandate is what keeps them readable - so a" >&2
  echo "       count that falls means the pattern stopped matching the header." >&2
  RESTART_CAUSE_FAIL=1
fi
if [ "$CAL_CAUSE_COUNT" -ne "$CAL_MEMBER_COUNT" ] || [ "$APP_CAUSE_COUNT" -ne "$APP_MEMBER_COUNT" ]; then
  echo "ERROR: BootDiag.h declares $CAL_MEMBER_COUNT RestartCause members of which $CAL_CAUSE_COUNT carry a" >&2
  echo "       number, and App/BootDiag.h declares $APP_MEMBER_COUNT of which $APP_CAUSE_COUNT do. One" >&2
  echo "       binary writes these numbers to NVS and the other reads them back, so every" >&2
  echo "       value is spelled out as '= N' in both files. A member taking its number" >&2
  echo "       from its position is compared by nothing here." >&2
  RESTART_CAUSE_FAIL=1
fi
if [ "$CAL_CAUSES" != "$APP_CAUSES" ]; then
  echo "ERROR: BootDiag.h and App/BootDiag.h disagree on RestartCause." >&2
  echo "       These values are the wire format between two binaries on one device:" >&2
  echo "       CAL writes the number to NVS before restarting, the App reads it on the" >&2
  echo "       next boot and reports it as the restart cause. A mismatch misreports" >&2
  echo "       exactly the restart that installed it, and no compiler will catch it." >&2
  echo "       Add the value in BOTH files with the SAME number, and never renumber." >&2
  echo "       See CAL_OTA_DESIGN.md section 13 and TEST_PLAN.md section 7a." >&2
  diff <(printf '%s\n' "$CAL_CAUSES") <(printf '%s\n' "$APP_CAUSES") >&2 || true
  RESTART_CAUSE_FAIL=1
fi
if [ "$RESTART_CAUSE_FAIL" -ne 0 ]; then
  exit 1
fi

# EVERY BOUNDED STRING GOES THROUGH THE ONE LAYOUT ROUTINE.
#
# Two failures photographed on real hardware on 2026-09-25, from two different
# pieces of code making the same mistake differently: a sports card reading
# "Houston Astr" because drawTruncatedLeft() shortened a team name with nothing
# to say it had, and a notice card reading "Test device: this rotation h"
# because the greedy wrap drew the very line it had just measured and rejected.
# App/Display.cpp had five different ways of putting a string into a bounded
# region. It now has one, layoutText(), and TEXT_LAYOUT_DESIGN.md says why.
#
# A compiler cannot catch a sixth appearing. `lcd.drawString(someWireString, x,
# y)` is perfectly legal and draws perfectly happily until the string is long,
# which is the day nobody is looking. So it is checked the only way it can be,
# the same way the Motion entry points above are: every drawString() call in
# App/Display.cpp must either pass a string literal, which is bounded because
# the programmer can see it, or carry the LAYOUT-PRIMITIVE sentinel that marks
# the few places allowed to draw raw - inside layoutText() itself, the corner
# clock, the clock card's hero, and drawTemperature's cursor walk.
#
# What is counted is the method name and the driver boundary, not the receiver.
# The pattern read lcd.drawString( and nothing else until 2026-11-03, which is a
# spelling rather than a route, and
#
#     auto& panel = lcd;
#     panel.drawString(whatToDo, 10, 200);
#
# added to showFailure() drew a server string into no box - the failure the
# paragraph above describes - while this gate read its five sites and passed.
#
# The receiver can be called anything. The route cannot. Canvas declares one
# text-drawing virtual, drawString(); LgfxCanvas is its one implementation and
# holds the one hand-off to LovyanGFX; and the block between the file's own
# "---- The driver" and "---- What the rest of this file draws on" markers is the
# only one that names the panel object, which is what leaves Canvas as the only
# way a string reaches the glass from anywhere else in the file. So both of those
# are read below, both are printed, and both carry a floor, because a pattern
# that stops matching returns nothing and an absence reads as a pass.
#
# A permit the compiler checks would be better: drawString() private on Canvas,
# taking a tag only the four sanctioned functions can construct, so an unbounded
# draw fails to build under any receiver name. That is a change to what a card
# can draw, so TEXT_LAYOUT_DESIGN.md goes first and that document is in the
# server tree. It would not retire this gate, because panelDevice carries
# LovyanGFX's own print() and drawCenterString() and a drawString permit says
# nothing about either; the boundary reading below is what holds those. What
# neither reading sees is a second text method added to Canvas, and that is the
# part only the compiler can hold.
echo "==> Checking every bounded string in App/Display.cpp goes through layoutText()"
display_draw_calls() {
  # Calls of a method named drawString on any receiver, named or through a
  # pointer. Canvas's own declaration and the forwarding call in its String
  # overload carry no receiver and match nothing here.
  grep -nE '[A-Za-z_][A-Za-z0-9_]*[[:space:]]*(\.|->)[[:space:]]*drawString[[:space:]]*\(' App/Display.cpp
}
DRAW_CALLS=$(display_draw_calls || true)
DRAW_CALL_COUNT=$(printf '%s\n' "$DRAW_CALLS" | grep -c . || true)
DRIVER_HANDOFF=$(printf '%s\n' "$DRAW_CALLS" | grep -c 'panelDevice\.drawString' || true)
# The driver's hand-off to LovyanGFX is not a call site. Everything else is.
CALL_SITES=$(printf '%s\n' "$DRAW_CALLS" | grep -v 'panelDevice\.drawString' || true)
CALL_SITE_COUNT=$(printf '%s\n' "$CALL_SITES" | grep -c . || true)
RAW_DRAWS=$(printf '%s\n' "$CALL_SITES" \
            | grep -v 'LAYOUT-PRIMITIVE' \
            | grep -vE '(\.|->)[[:space:]]*drawString[[:space:]]*\("' || true)
RAW_DRAW_COUNT=$(printf '%s\n' "$RAW_DRAWS" | grep -c . || true)
SANCTIONED_COUNT=$((CALL_SITE_COUNT - RAW_DRAW_COUNT))
echo "    $DRAW_CALL_COUNT drawString calls read out of App/Display.cpp: $DRIVER_HANDOFF the driver's hand-off, $SANCTIONED_COUNT of $CALL_SITE_COUNT call sites sanctioned"

TEXT_DRAW_FAIL=0
if [ "$DRAW_CALL_COUNT" -eq 0 ]; then
  echo "ERROR: no drawString call was found in App/Display.cpp, so this gate read" >&2
  echo "       nothing at all. The pattern above stopped matching the file." >&2
  TEXT_DRAW_FAIL=1
elif [ "$DRAW_CALL_COUNT" -lt 6 ]; then
  echo "ERROR: only $DRAW_CALL_COUNT drawString calls were read out of App/Display.cpp, and there" >&2
  echo "       were 6 on 2026-11-03 - five call sites and the driver's one hand-off." >&2
  echo "       Either call sites went away, or Canvas's text virtual was renamed and" >&2
  echo "       what is counted here is no longer the route a string takes to the glass." >&2
  TEXT_DRAW_FAIL=1
fi
if [ "$DRIVER_HANDOFF" -ne 1 ]; then
  echo "ERROR: App/Display.cpp names panelDevice.drawString() on $DRIVER_HANDOFF lines, and LgfxCanvas" >&2
  echo "       is the one implementation of Canvas's one text virtual. A second hand-off" >&2
  echo "       is a second route to the glass, and the sanction above reads neither of" >&2
  echo "       them as a call site. See TEXT_LAYOUT_DESIGN.md section 7." >&2
  TEXT_DRAW_FAIL=1
fi
if [ -n "$RAW_DRAWS" ]; then
  echo "ERROR: App/Display.cpp draws a non-literal string without going through" >&2
  echo "       layoutText(). Such a string is measured against nothing and is clipped" >&2
  echo "       by the panel when it is too long, which is how 'Houston Astros' reached" >&2
  echo "       a household's wall reading 'Houston Astr'." >&2
  echo "$RAW_DRAWS" >&2
  echo "       Use layoutText()/layoutLine() with a TextBox, or, if this genuinely" >&2
  echo "       cannot be expressed as a box, say why in a comment and add the" >&2
  echo "       LAYOUT-PRIMITIVE sentinel. See TEXT_LAYOUT_DESIGN.md section 7." >&2
  TEXT_DRAW_FAIL=1
fi

# The boundary the reading above rests on. The declaration is counted as well as
# the references, because "panelDevice is named nowhere outside the driver" is an
# absence check and reads clean the moment the object is called something else.
DRIVER_START=$(grep -n '^// ---- The driver' App/Display.cpp | head -1 | cut -d: -f1 || true)
DRIVER_END=$(grep -n '^// ---- What the rest of this file draws on' App/Display.cpp | head -1 | cut -d: -f1 || true)
if [ -z "$DRIVER_START" ] || [ -z "$DRIVER_END" ]; then
  echo "ERROR: App/Display.cpp no longer carries both of the section markers that bound" >&2
  echo "       the driver, '// ---- The driver' and '// ---- What the rest of this file" >&2
  echo "       draws on', so there is no boundary here to read anything against." >&2
  TEXT_DRAW_FAIL=1
else
  PANEL_DECL=$(sed -n "${DRIVER_START},${DRIVER_END}p" App/Display.cpp | grep -c '^LGFX panelDevice;' || true)
  PANEL_INSIDE=$(sed -n "${DRIVER_START},${DRIVER_END}p" App/Display.cpp | grep -c 'panelDevice' || true)
  PANEL_TOTAL=$(grep -c 'panelDevice' App/Display.cpp || true)
  PANEL_OUTSIDE=$((PANEL_TOTAL - PANEL_INSIDE))
  echo "    panelDevice named on $PANEL_INSIDE lines inside the driver block (lines $DRIVER_START-$DRIVER_END), $PANEL_OUTSIDE outside it"
  if [ "$PANEL_DECL" -ne 1 ]; then
    echo "ERROR: the driver block of App/Display.cpp does not declare 'LGFX panelDevice;'," >&2
    echo "       so the figure beside it counts a name this file no longer uses and reads" >&2
    echo "       zero wherever the panel object now lives. Declare it there, or teach" >&2
    echo "       this gate the new name." >&2
    TEXT_DRAW_FAIL=1
  elif [ "$PANEL_INSIDE" -lt 20 ]; then
    echo "ERROR: panelDevice is named on only $PANEL_INSIDE lines inside the driver block, and on 38" >&2
    echo "       on 2026-11-03. The floor sits well below that figure because the shape" >&2
    echo "       wrappers come and go, so a reading this low means the block moved out" >&2
    echo "       from between the two markers rather than that it got smaller." >&2
    TEXT_DRAW_FAIL=1
  fi
  if [ "$PANEL_OUTSIDE" -ne 0 ]; then
    echo "ERROR: App/Display.cpp names panelDevice outside the driver block:" >&2
    grep -n 'panelDevice' App/Display.cpp \
      | awk -F: -v s="$DRIVER_START" -v e="$DRIVER_END" '$1 < s || $1 > e' >&2 || true
    echo "       That reaches LovyanGFX straight, which skips Canvas and so skips every" >&2
    echo "       reading above - panelDevice carries print(), setCursor() and" >&2
    echo "       drawCenterString(), none of which any pattern here counts. The panel" >&2
    echo "       object stays in the driver block and the rest of the file draws on" >&2
    echo "       Canvas. See TEXT_LAYOUT_DESIGN.md section 7." >&2
    TEXT_DRAW_FAIL=1
  fi
fi
if [ "$TEXT_DRAW_FAIL" -ne 0 ]; then
  exit 1
fi

# The helpers layoutText() replaced, by name. Bringing any of them back would
# compile and would silently reinstate the defect, so the names are retired
# rather than deprecated - a deprecated helper carrying a comment asking people
# not to use it is a helper that gets used, and three of these were themselves
# the same truncation loop written out separately.
#
# The scan is App/ only. CAL's own root Display.cpp keeps a wrappedCenteredText,
# corrected in the same pass but deliberately not replaced by a layout module:
# the factory partition has one call site shape and nothing to spend it on.
echo "==> Checking the retired text helpers have not come back"
for RETIRED in drawTruncatedLeft drawRightJustified wrappedLeftText wrappedCenteredText; do
  # Definitions and calls, not the comments in Display.cpp and in this file
  # that explain what they were, so the pattern needs a '(' straight after the
  # name and skips a line that starts as a comment.
  if grep -rnE "^[^/]*\b${RETIRED}\(" --exclude-dir=build --include='*.cpp' --include='*.h' --include='*.ino' App/ >/dev/null 2>&1; then
    echo "ERROR: ${RETIRED}() is back in App/." >&2
    echo "       It was removed because it shortened server-supplied text with no" >&2
    echo "       indication that anything had been lost. Use layoutText()/layoutLine()." >&2
    echo "       See TEXT_LAYOUT_DESIGN.md section 7." >&2
    grep -rnE "^[^/]*\b${RETIRED}\(" --exclude-dir=build --include='*.cpp' --include='*.h' --include='*.ino' App/ >&2
    exit 1
  fi
done

# The third text gate. A card leaving a text scale selected hands it to the
# next screen, which is how a scale of 2 set for a boot headline reached the
# sports board. Its own script, beside check-partition-fit.sh, because it is an
# awk over function bodies rather than one more grep and because it has to be
# runnable on its own: there is no host compiler on the workstation this
# firmware is written on, so ci/test-recovery-menu.sh cannot run there and this
# check and the two above are what can be run in a second rather than in a
# twenty-minute compile.
#
# Via bash explicitly, for the reason check-partition-fit.sh's own invocation
# below gives: a script committed from a Windows filesystem does not reliably
# carry its executable bit into git.
bash ci/check-text-state.sh

# The fourth. The scoreboard lays one grid for the whole board, sized against
# the widest status string on it, and App/CardColumns.h's hundred and five
# static_asserts and the host sweep over them read none of App/Display.cpp, so
# the arguments the draw path hands that arithmetic, and where in the function
# it hands them, are the one part of the layout with nothing checking it. Its
# own script for the same reason as the gate above.
bash ci/check-board-columns-call.sh

# The fifth. A score below zero is not a scoreline, so CheckIn.cpp stores
# Sports::kNoScore for it and both formatters in Sports.cpp draw an empty cell.
# What that buys showSportsBoard() is an invariant - every score string is
# decimal digits with no sign - and the fold keys on the length of that string,
# so a `-10` would be three characters and would drop every score on the board a
# face. The predicate is swept over the whole int16_t range on a host; none of
# the four files that define or call it compiles there, so the call sites are
# this gate's. TEXT_LAYOUT_DESIGN.md section 15m.
bash ci/check-score-guard.sh

# The sixth. Assets.cpp keeps two counters whose values leave the device - one
# on the check-in request, one in a log line - and App/ReportedCounter.h's host
# sweep past the end of a uint8_t reads neither call site. A bare ++ on either
# compiles, links, passes every host test and reports a wrapped figure, and at
# exactly 256 occurrences the decode-failure report reads as nothing to send.
# TEST_PLAN.md section 21.
bash ci/check-asset-counters.sh

# The seventh. CardManager.cpp resolves what a draw reports three ways in order
# and names a graphics library, so it does not compile on a host and the order it
# applies is held as text, along with the registry's own check on the key policy
# a registration states. Which cards owe a key is the compiler's as of
# 2026-11-03 - Cards::registerCard() takes a KeyPolicy with no default, and
# ci/test-key-policy.sh proves an omitted one does not build - so this gate no
# longer carries a list of them.
# CARD_IMPRESSIONS_DESIGN.md, "Which attribute says what the draw was about".
bash ci/check-content-key-resolvers.sh

# The eighth, and the only one that reads tests/ rather than App/. The host tests
# build with whatever g++ the machine has, so a stub standing in for a POSIX
# function with a Microsoft one compiles here and not on ubuntu-latest. This runs
# where the difference is invisible, which is the machine that has the Microsoft
# interface to reach for.
bash ci/check-host-test-portability.sh

echo "==> Compiling CAL for the ESP32-2432S028"
arduino-cli compile --fqbn "$FQBN" --libraries "$LIBRARIES_DIR" --export-binaries .

echo "==> Compiling App for the ESP32-2432S028"
(
  cd App
  arduino-cli compile --fqbn "$FQBN" --libraries "../$LIBRARIES_DIR" --export-binaries .
)

# arduino-cli's own "Sketch uses X of Y bytes" line checks the FQBN's generic
# min_spiffs scheme (1,966,080 bytes), not the real, asymmetric partitions.csv
# actually baked into the binary - a build well within the true factory or
# ota_0 ceiling can look alarming there, and worse, a build that has genuinely
# grown past the real ceiling still reports "fits" against the wrong number.
# The comparison is ci/check-partition-fit.sh, which reads the table out of the
# build output directory beside the image rather than a filename in the sketch
# root. That is the table this image was compiled against, so the same check
# covers a board whose table is named something else and is selected by being
# copied over partitions.csv for the length of one compile - the two
# compile-*-s3.cmd wrappers call this same script for exactly that reason, and
# a ceiling read from the sketch root afterwards would be the other board's.
# Reading a table also keeps the ceilings out of this file, so resizing a
# partition does not leave a constant here to drift.
echo "==> Verifying compiled size against the real partition table"
# Invoked via bash explicitly rather than ./ci/check-partition-fit.sh: a script
# committed from a Windows filesystem does not reliably carry its executable
# bit into git, and this way it does not matter.
CHECK_FIT="bash ci/check-partition-fit.sh"
$CHECK_FIT "${BUILD_DIR}/CAL.ino.bin" factory "CAL (factory)"
$CHECK_FIT "App/${BUILD_DIR}/App.ino.bin" ota_0 "App (ota_0)"

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
$CHECK_FIT "${BUILD_DIR}/CAL.ino.bin" ota_0 "CAL (must also fit ota_0, for OTA staging)"

# AND AGAINST THE TABLE THE FLEET IS ACTUALLY RUNNING, WHICH IS NOT THIS ONE.
#
# partitions.csv describes the INTENDED layout - factory 1,703,936, ota_0 at
# 0x1B0000, with a callog partition. Device 17's flash was read directly on
# 2026-09-16 and reports the OLD layout: factory 1,441,792, ota_0 at 0x170000,
# no callog. The re-table was prepared but never applied, and that is not an
# oversight - changing a partition table requires USB, and the CAL reflash went
# out through the browser flasher, which cannot rewrite one.
#
# So every check above validates against a ceiling that does not exist in the
# field. A CAL between 1,441,792 and 1,703,936 bytes would pass this script and
# then not fit the flash of any device currently deployed. The build would be
# green and the flashing would fail.
#
# 1,441,792 is hardcoded rather than read from a file on purpose: there is no
# file in this repository that describes the field layout, and inventing one
# would just be a second thing to drift. Nothing server-side records a device's
# table either - filed separately - so this number came from reading 0x8000 on a
# real unit and is the only evidence there is.
#
# It is passed as an explicit byte count rather than a partition name because
# it is not a partition in any table here, and it applies to this board only:
# the 4MB units in the field are the ones carrying that layout.
#
# DELETE THIS CHECK when the USB pass has re-tabled the fleet, and not before.
# Until then it is the binding constraint and the other one is aspirational.
FIELD_FACTORY_CEILING=1441792
$CHECK_FIT "${BUILD_DIR}/CAL.ino.bin" "bytes:${FIELD_FACTORY_CEILING}" "CAL (field factory, pre-re-table)"

# =====================================================================
# THE PANELLAN ZX2D80CE02S, which is the other board and the other table
# =====================================================================
#
# Until 2026-10-28 this script built one board and the S3 target existed only in
# compile-cal-s3.cmd and compile-app-s3.cmd on one workstation. A real Panellan
# in the field was therefore offered nothing at all, and before its board was
# declared server-side it was offered the ESP32 CAL and the ESP32 App, failed to
# install both and invalidated its ota_0.
#
# THE PARTITION TABLE IS SWAPPED IN AND OUT HERE, exactly as the two .cmd
# wrappers do it and for the same reason: Arduino reads the partitions.csv
# sitting beside the sketch and there is only one such name, so selecting a
# board's table means putting it there. BOTH copies move, the root one and
# App/partitions.csv, because the "two partition tables agree" gate above diffs
# them against each other and App.ino.merged.bin bakes its copy in at 0x8000 -
# a stale one re-tables any device flashed with that image.
#
# FQBN notes, and none of these four is optional:
#   PSRAM=enabled   QSPI. The module is a WROVER-N8R2 and the installed S3
#                   libraries are CONFIG_SPIRAM_MODE_QUAD, re-verified against
#                   the generated sdkconfig of both S3 builds on 2026-10-27.
#   CDCOnBoot=cdc   The S3 has native USB. Without this Serial goes nowhere and
#                   a running board looks dead.
#   PartitionScheme=custom
#                   Declares build.partitions empty and lifts
#                   upload.maximum_size to 16777216, so the sketch's own table
#                   is used and the min_spiffs ceiling of 1966080 does not
#                   refuse a valid build. A 16MB ceiling accepts anything this
#                   board can produce, so arduino-cli will not refuse an
#                   oversized image - check-partition-fit.sh is what holds
#                   these two to their own table.
#   build.flash_size=8MB
#                   The generic esp32s3 board has NO FlashSize menu and hard
#                   defaults to 4MB. Left at 4MB the image header disagrees with
#                   an 8MB table and partitions past 4MB are rejected.
# The committed-tables check already ran near the top of this script, before the
# 4MB compiles, so a tree left holding the 8MB table never reaches a compile at
# all. Re-asserted here because what follows is what makes that check necessary.
require_committed_partition_tables

# Restored on any exit this script can see: a failed compile, a failed size check,
# a failed rename, and SIGINT or SIGTERM. EXIT alone does not cover a ctrl-C -
# bash dying from an uncaught SIGINT runs no EXIT trap - so INT and TERM are
# trapped by name, and each of those arms EXITS rather than returning. A handler
# that restores and returns would hand control back to the line after the
# interrupted compile, which would then build the S3 target against the 4MB table
# just put back: worse than not trapping the signal at all. A kill -9 is still
# outside this, and the committed-tables check above is what catches that on the
# next run.
#
# The restore comes out of git rather than out of a backup copy, so a corrupt
# backup cannot pass it, and a restore that did not take fails the script even
# when everything before it succeeded: an 8MB table left under the 4MB name
# produces an App.ino.merged.bin that re-tables whatever device it is flashed to,
# and nothing between here and that device reports anything wrong.
#
# Idempotent, because the success path calls it directly before the checksums so
# the log reads in order, and the trap then runs it a second time on the way out.
restore_fleet_partition_tables() {
  local rc=$?

  if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "ERROR: cannot restore the 4MB partition tables - this is not a git work tree." >&2
    echo "       Whatever is in partitions.csv and App/partitions.csv now is what is there." >&2
    exit 1
  fi

  git checkout -- partitions.csv App/partitions.csv 2>/dev/null || true

  if ! git diff --quiet HEAD -- partitions.csv App/partitions.csv; then
    echo "ERROR: the 4MB partition tables were not restored." >&2
    echo "       partitions.csv and/or App/partitions.csv still differ from HEAD. Fix them" >&2
    echo "       before any other build - an 8MB table under the 4MB name produces an" >&2
    echo "       App.ino.merged.bin that re-tables whatever it is flashed to." >&2
    exit 1
  fi

  return $rc
}
trap restore_fleet_partition_tables EXIT
trap 'restore_fleet_partition_tables; echo "interrupted (SIGINT)" >&2; exit 130' INT
trap 'restore_fleet_partition_tables; echo "terminated (SIGTERM)" >&2; exit 143' TERM

# THE PREVIOUS RUN'S ARTIFACTS GO FIRST, so a stem that stops being produced
# fails the rename instead of being published from the last build. The build
# directories are not cleaned by arduino-cli and are not cleaned between runs:
# a .bin left there from an earlier compile is copied to its published name,
# summed under that name, and uploaded, with the script exiting 0. CI runs on a
# fresh checkout so this is a workstation and .cmd-wrapper hazard, which is
# exactly where the artifacts of two different builds get mixed into one
# checksummed set. Only the exported images are removed, not the object cache,
# so this costs nothing in compile time.
echo "==> Clearing the previous Panellan artifacts"
rm -f "$S3_BUILD_DIR"/*.bin "App/$S3_BUILD_DIR"/*.bin

echo "==> Swapping in the 8MB table"
cp -f "$PANELLAN_TABLE" partitions.csv
cp -f "App/$PANELLAN_TABLE" App/partitions.csv

# Copied from each sketch's OWN copy rather than both from the root one. The two
# are byte-identical and the PAIR is what the gate above checks: root
# partitions-panellan-8mb.csv against App's. Building App from the root file
# would leave this board's App-side table unchecked while looking fine.
echo "==> Checking the two 8MB partition tables agree"
if ! diff -q partitions.csv App/partitions.csv >/dev/null; then
  echo "ERROR: $PANELLAN_TABLE and App/$PANELLAN_TABLE differ." >&2
  diff partitions.csv App/partitions.csv >&2 || true
  exit 1
fi

echo "==> Compiling CAL for the Panellan ZX2D80CE02S"
arduino-cli compile --fqbn "$S3_FQBN" --build-property build.flash_size=8MB \
  --libraries "$LIBRARIES_DIR" --export-binaries .

echo "==> Compiling App for the Panellan ZX2D80CE02S"
(
  cd App
  arduino-cli compile --fqbn "$S3_FQBN" --build-property build.flash_size=8MB \
    --libraries "../$LIBRARIES_DIR" --export-binaries .
)

# Measured against the table in each image's OWN build directory, which is the
# 8MB table these two compiles used - arduino-cli copies it there. So the check
# reads the right ceiling even though the trap above will have put the 4MB table
# back by the time anybody reads the log.
#
# factory and ota_0 for CAL, for the CAL_OTA_DESIGN.md section 5.4 reason the 4MB
# pair is checked against both. ota_0 only for the App. The pre-re-table field
# ceiling is NOT checked here: that number is the layout on the deployed 4MB
# units and says nothing about this board.
echo "==> Verifying the Panellan images against the table those builds used"
$CHECK_FIT "${S3_BUILD_DIR}/CAL.ino.bin" factory "CAL Panellan (factory)"
$CHECK_FIT "${S3_BUILD_DIR}/CAL.ino.bin" ota_0 "CAL Panellan (must also fit ota_0, for OTA staging)"
$CHECK_FIT "App/${S3_BUILD_DIR}/App.ino.bin" ota_0 "App Panellan (ota_0)"

# A GITHUB RELEASE ASSET IS ADDRESSED BY ITS BASENAME ALONE, so the two boards'
# files cannot share one. Uploading a second file under a name already present
# turns into an "update" of the first, which raced GitHub's API and 404'd when
# this script's two checksums.txt were both listed - see the release steps in
# .github/workflows/build-and-release.yml.
#
# So the Panellan's artifacts are copied to suffixed names and those are what the
# release publishes. The 4MB board's names do NOT move: every release published
# to date carries them, https://discoveraroundme.com/cal hands CAL.ino.merged.bin
# to a household, and the server's catalog resolves CAL.ino.bin and App.ino.bin.
# Renaming them makes every existing release unresolvable at once.
#
# Copied rather than renamed so check-partition-fit.sh's own output above, and
# anything else reading the build directory by arduino-cli's names, keeps working.
# RESTORED HERE, not only by the trap on the way out. The trap runs after the
# last line of this script, so on the success path its output landed after
# "==> Done." and a reader skimming the tail saw a completion banner followed by
# the restore - or, when the restore failed, a completion banner followed by the
# error. Calling it directly puts the restore and its verification where they
# happened. The trap stays for every path that does not reach this line, and the
# function is idempotent so running twice costs a no-op git checkout.
echo "==> Restoring the 4MB partition tables"
restore_fleet_partition_tables
echo "    restore verified"

echo "==> Naming the Panellan artifacts for the release"
for STEM in CAL.ino CAL.ino.merged CAL.ino.bootloader CAL.ino.partitions; do
  cp -f "${S3_BUILD_DIR}/${STEM}.bin" "${S3_BUILD_DIR}/${STEM}.${PANELLAN_WIRE}.bin"
  echo "    ${STEM}.${PANELLAN_WIRE}.bin"
done
for STEM in App.ino App.ino.merged App.ino.bootloader App.ino.partitions; do
  cp -f "App/${S3_BUILD_DIR}/${STEM}.bin" "App/${S3_BUILD_DIR}/${STEM}.${PANELLAN_WIRE}.bin"
  echo "    ${STEM}.${PANELLAN_WIRE}.bin"
done

echo "==> Computing checksums"
(
  cd "$BUILD_DIR"
  sha256sum CAL.ino.merged.bin CAL.ino.bin CAL.ino.bootloader.bin CAL.ino.partitions.bin > checksums.txt
)
(
  cd "App/$BUILD_DIR"
  sha256sum App.ino.merged.bin App.ino.bin App.ino.bootloader.bin App.ino.partitions.bin > checksums.txt
)
# THE PANELLAN'S ARE SUMMED UNDER THEIR PUBLISHED NAMES, not arduino-cli's. The
# combined file is what somebody checks a download against, so the name in it has
# to be the name of the file they downloaded. Summing the unsuffixed copies would
# print four lines that read as the 4MB board's and contradict the other half of
# this file.
(
  cd "$S3_BUILD_DIR"
  sha256sum \
    "CAL.ino.merged.${PANELLAN_WIRE}.bin" \
    "CAL.ino.${PANELLAN_WIRE}.bin" \
    "CAL.ino.bootloader.${PANELLAN_WIRE}.bin" \
    "CAL.ino.partitions.${PANELLAN_WIRE}.bin" > checksums.txt
)
(
  cd "App/$S3_BUILD_DIR"
  sha256sum \
    "App.ino.merged.${PANELLAN_WIRE}.bin" \
    "App.ino.${PANELLAN_WIRE}.bin" \
    "App.ino.bootloader.${PANELLAN_WIRE}.bin" \
    "App.ino.partitions.${PANELLAN_WIRE}.bin" > checksums.txt
)
# One combined file is what actually goes in a release's notes/body and is
# published as its own downloadable asset - a release with four separate
# checksums.txt files floating among its other assets would just mean
# guessing which one covers which binary, and all four share one basename so
# only the first could be uploaded anyway.
cat \
  "$BUILD_DIR/checksums.txt" \
  "App/$BUILD_DIR/checksums.txt" \
  "$S3_BUILD_DIR/checksums.txt" \
  "App/$S3_BUILD_DIR/checksums.txt" | tee combined-checksums.txt

echo "==> Done."
echo "    ESP32-2432S028: ${BUILD_DIR}/ and App/${BUILD_DIR}/"
echo "    Panellan ZX2D80CE02S: ${S3_BUILD_DIR}/ and App/${S3_BUILD_DIR}/"
