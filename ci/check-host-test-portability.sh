#!/usr/bin/env bash
# Holds the host tests and their stubs to interfaces both runners have.
#
# The host tests build with whatever g++ the machine has: MinGW-w64 on the
# workstation this firmware is written on, glibc and GCC 13 on ubuntu-latest.
# A stub reaching for a Microsoft-only function compiles on the first and not on
# the second, so the test passes locally and fails on the runner. On the release
# lane a failing host test skips every later step, so what that costs is the
# release.
#
# The shape is narrow enough to check as text. Every name below is in Microsoft's
# C runtime and not in glibc, and two of them collide with a POSIX function of a
# near-identical name: gmtime_r and localtime_r take their arguments the other
# way round and answer a pointer, and glibc declares both noexcept, so even a
# shim guarded only by "does gmtime_s exist" clashes on the exception specifier.
# Inside an "#ifdef _WIN32" block any of these is fine, because that block is
# the Windows half of a stub that has a POSIX half. Outside one it is a CI
# failure waiting for a push.
#
# A text gate rather than a test, in the same spirit as
# ci/check-content-key-resolvers.sh and ci/check-score-guard.sh beside it: what
# it checks is the absence of something, and the only machine that could prove it
# by compiling is the one that does not have the problem.
#
# What this does not catch is a Windows-only interface nobody has met yet, or a
# difference in behaviour rather than in availability. It holds the seven names
# that have cost something or are one keystroke from a POSIX function.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

# Microsoft's secure-CRT spellings, plus the underscore-prefixed snprintf that
# predates the C99 one and truncates without terminating.
INTERFACES="gmtime_s localtime_s ctime_s asctime_s strcpy_s sprintf_s _snprintf"

mapfile -t FILES < <(find tests -type f \( -name '*.h' -o -name '*.cpp' -o -name '*.hpp' \) | sort)
mapfile -t STUB_DIRS < <(find tests -type d -name '*stubs*' | sort)

FILE_COUNT=${#FILES[@]}
STUB_DIR_COUNT=${#STUB_DIRS[@]}

# How much was read, printed whether or not anything failed. A gate whose find
# stopped matching passes by scanning nothing, and the count is the only way to
# see that from the outside.
echo "    $FILE_COUNT source files under tests/, across $STUB_DIR_COUNT stub directories"

FAIL=0
if [ "$STUB_DIR_COUNT" -lt 5 ]; then
  echo "ERROR: only $STUB_DIR_COUNT stub directories were found and there were 5 on" >&2
  echo "       2026-11-03. Either stubs were removed, or the find above stopped matching" >&2
  echo "       and this gate read less than tests/ holds." >&2
  FAIL=1
fi
if [ "$FILE_COUNT" -lt 20 ]; then
  echo "ERROR: only $FILE_COUNT source files were found under tests/ and there were 20 on" >&2
  echo "       2026-11-03. Either tests were retired, or the find above stopped matching." >&2
  FAIL=1
fi
if [ "$FILE_COUNT" -eq 0 ]; then
  exit 1
fi

# Comments go before the search. These stubs explain which Windows function they
# stand in for, by name, so a scan of the raw text reports every remark as a
# defect. Block comments span lines, so the state carries across records.
#
# Then the preprocessor conditionals, as a stack: a level is Windows-only when
# its #if or #ifdef names _WIN32 without negating it, an #else flips that level,
# and a name is exempt when any level still open is Windows-only. \r is stripped
# first, because a carriage return at the end of a directive stops every pattern
# below from matching, which would let the gate pass by recognizing nothing.
REPORT=$(awk -v names="$INTERFACES" '
  BEGIN {
    count = split(names, name, " ")
    inblock = 0
  }
  FNR == 1 { inblock = 0; depth = 0 }
  {
    line = $0
    sub(/\r$/, "", line)

    code = ""
    i = 1
    len = length(line)
    while (i <= len) {
      two = substr(line, i, 2)
      if (inblock) {
        if (two == "*/") { inblock = 0; i += 2 } else { i++ }
        continue
      }
      if (two == "/*") { inblock = 1; i += 2; continue }
      if (two == "//") { break }
      code = code substr(line, i, 1)
      i++
    }

    if (code ~ /^[ \t]*#[ \t]*(if|ifdef|ifndef)[ \t(!]/) {
      depth++
      gated[depth] = 0
      kind[depth] = "other"
      if (code ~ /_WIN32/) {
        if (code ~ /^[ \t]*#[ \t]*ifndef/ || code ~ /![ \t]*defined/) {
          kind[depth] = "negative"
        } else {
          kind[depth] = "positive"
          gated[depth] = 1
        }
      }
      next
    }
    if (code ~ /^[ \t]*#[ \t]*(else|elif)/) {
      if (depth > 0) {
        if (kind[depth] == "positive") { gated[depth] = 0 }
        else if (kind[depth] == "negative") { gated[depth] = 1 }
        if (code ~ /^[ \t]*#[ \t]*elif/ && code ~ /_WIN32/ && code !~ /![ \t]*defined/) {
          kind[depth] = "positive"
          gated[depth] = 1
        }
      }
      next
    }
    if (code ~ /^[ \t]*#[ \t]*endif/) {
      if (depth > 0) { depth-- }
      next
    }

    windows_only = 0
    for (d = 1; d <= depth; d++) {
      if (gated[d] == 1) { windows_only = 1 }
    }
    if (windows_only) { next }

    for (k = 1; k <= count; k++) {
      if (code ~ ("(^|[^A-Za-z0-9_])" name[k] "([^A-Za-z0-9_]|$)")) {
        printf "%s:%d:%s:%s\n", FILENAME, FNR, name[k], code
      }
    }
  }
' "${FILES[@]}")

if [ -n "$REPORT" ]; then
  echo "$REPORT" | while IFS=: read -r file lineno interface code; do
    echo "ERROR: $file:$lineno reaches for $interface outside an #ifdef _WIN32 block." >&2
    echo "       $(printf '%s' "$code" | sed 's/^[ \t]*//')" >&2
    echo "       That name is in Microsoft's C runtime and not in glibc, so this file" >&2
    echo "       compiles here and does not compile on ubuntu-latest. Put it behind" >&2
    echo "       '#ifdef _WIN32' with the POSIX call in the #else, the way" >&2
    echo "       tests/announcement-and-home-value-stubs/Arduino.h holds gmtime_r." >&2
  done
  FAIL=1
fi

if [ "$FAIL" -ne 0 ]; then
  exit 1
fi

echo "PASS: host test stubs use no Windows-only interface outside an #ifdef _WIN32 block"
