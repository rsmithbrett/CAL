#!/usr/bin/env bash
# Holds the host tests and their stubs to interfaces both runners have.
#
# The host tests build with whatever g++ the machine has: MinGW-w64 on the
# workstation this firmware is written on, glibc and GCC 13 on ubuntu-24.04. A
# stub reaching for a Microsoft-only function compiles on the first and not on
# the second, so the test passes locally and fails on the runner. On the release
# lane a failing host test skips every later step, so what that costs is the
# release.
#
# Every name below is in Microsoft's C runtime and not in glibc, and two of them
# collide with a POSIX function of a near-identical name: gmtime_r and
# localtime_r take their arguments the other way round and answer a pointer, and
# glibc declares both noexcept, so even a shim guarded only by "does gmtime_s
# exist" clashes on the exception specifier. Inside a block guarded on _WIN32
# any of these is fine, because that block is the Windows half of a stub that
# has a POSIX half. Outside one it is a CI failure waiting for a push.
#
# What decides this is the compiler on ubuntu-24.04. It reads the macros,
# expands them and evaluates the conditionals, which is what no text scan does,
# and its answer arrives in the host test step. This script asks the same
# question earlier, on the machine that has the Microsoft interface to reach
# for, and answers it in one line rather than a wall of g++ output naming a
# symbol and not a reason. That is its job: it earns its keep from
# ci/build-firmware.sh before a push more than it does as a second opinion in
# CI, and it stays narrow rather than becoming a preprocessor written in awk.
#
# What it reads: every file under tests/ with a C or C++ extension, with
# comments, string literals and character literals taken out, and the
# preprocessor conditionals tracked as a stack. A level is exempt when its whole
# condition is one _WIN32 test, so `#ifdef _WIN32`, `#if defined(_WIN32)` and
# `#if _WIN32` exempt their bodies, while the `#else` of one of those and the
# bodies of the negated spellings are read. A name on an `#if` or `#elif` line
# is read too, except as the operand of `defined`, which asks whether a name
# exists rather than calling it.
#
# It prints how many files it read out of how many sit under tests/, and names
# any it did not read. A script whose find stopped matching passes by reading
# nothing, and those counts are the only way to see that from the outside. A
# file whose extension is in neither list below fails this script rather than
# going unread.
#
# What it does not catch, each of which is the compiler's to find in the host
# test step this one runs ahead of:
#
# - A name assembled rather than written, by `a##b` token pasting or through a
#   typedef, an alias or a function pointer.
# - A Microsoft-only interface that is not one of the seven names below.
# - A difference in behaviour rather than in availability.
# - A raw string literal, R"(...)", whose contents read here as code.
# - A conditional mixing a _WIN32 test with another term. That level is read
#   rather than exempted, so nest it to exempt its body.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

# Microsoft's secure-CRT spellings, plus the underscore-prefixed snprintf that
# predates the C99 one and truncates without terminating.
INTERFACES="gmtime_s localtime_s ctime_s asctime_s strcpy_s sprintf_s _snprintf"

# The extensions a C or C++ header or translation unit arrives under, and the
# extensions that hold no code. One enumeration of tests/ is split between the
# two, so every file is either read or named, and a third spelling fails rather
# than passing by going unread.
SOURCE_EXT_RE='\.(h|hpp|hh|hxx|c|cc|cxx|cpp|inc|ipp)$'
NOT_SOURCE="md txt json csv yml yaml sh py png bin"

mapfile -t ALL_FILES < <(find tests -type f | sort)
mapfile -t FILES < <(printf '%s\n' "${ALL_FILES[@]}" | grep -E "$SOURCE_EXT_RE" || true)
mapfile -t STUB_DIRS < <(find tests -type d -name '*stubs*' | sort)

TOTAL_COUNT=${#ALL_FILES[@]}
STUB_DIR_COUNT=${#STUB_DIRS[@]}

if [ "${#FILES[@]}" -eq 0 ]; then
  echo "ERROR: no C or C++ source was found under tests/, so nothing was read." >&2
  exit 1
fi

# Comments and literals go before the search. These stubs name the Windows
# function they stand in for, in a remark or in a log string, and a scan of the
# raw text reports every one of those as a defect. A string literal holding /*
# would also open a block comment that never closes, which stops the scan
# finding anything in the rest of the file.
#
# Then the conditionals, as a stack: a level is Windows-only when its whole
# condition is one _WIN32 test, an #else flips that level, and a name is exempt
# when any level still open is Windows-only. \r is stripped per line, because a
# carriage return at the end of a directive stops every pattern below from
# matching, and this script would then pass by recognizing nothing.
#
# The scan reports its own file count on a #scanned: line, so the figure printed
# is what it opened rather than what find handed it.
RAW=$(awk -v names="$INTERFACES" '
  function trim(s) {
    sub(/^[ \t]+/, "", s)
    sub(/[ \t]+$/, "", s)
    return s
  }

  # The line with comments, string literals and character literals removed,
  # each literal leaving a space so the tokens either side stay apart. inblock
  # carries an unclosed /* to the next line and clears at every file.
  #
  # A quote whose partner is not on the same line stays in the output as itself
  # rather than taking the rest of the line with it, which is what keeps a digit
  # separator inside a numeric literal from hiding the code after it.
  function stripped(line,   out, i, j, len, two, ch, peek, quote, closed) {
    out = ""
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
      ch = substr(line, i, 1)
      if (ch == "\"" || ch == APOS) {
        quote = ch
        closed = 0
        j = i + 1
        while (j <= len) {
          peek = substr(line, j, 1)
          if (peek == "\\") { j += 2; continue }
          if (peek == quote) { closed = 1; break }
          j++
        }
        if (closed) {
          out = out " "
          i = j + 1
        } else {
          out = out ch
          i++
        }
        continue
      }
      out = out ch
      i++
    }
    return out
  }

  function gatedRange(lo, hi,   d) {
    for (d = lo; d <= hi; d++) {
      if (gated[d] == 1) { return 1 }
    }
    return 0
  }

  # positive when the whole condition is one _WIN32 test, negative when it is
  # that test negated, other for everything else, a condition mixing the test
  # with another term among them.
  function guardKind(keyword, cond,   c) {
    c = trim(cond)
    if (keyword == "ifdef") { return (c == "_WIN32") ? "positive" : "other" }
    if (keyword == "ifndef") { return (c == "_WIN32") ? "negative" : "other" }
    if (c == "_WIN32") { return "positive" }
    if (c ~ /^defined[ \t]*\([ \t]*_WIN32[ \t]*\)$/) { return "positive" }
    if (c ~ /^defined[ \t]+_WIN32$/) { return "positive" }
    if (c ~ /^![ \t]*_WIN32$/) { return "negative" }
    if (c ~ /^![ \t]*defined[ \t]*\([ \t]*_WIN32[ \t]*\)$/) { return "negative" }
    if (c ~ /^![ \t]*defined[ \t]+_WIN32$/) { return "negative" }
    return "other"
  }

  # The condition with every defined operand removed, and nothing at all for the
  # #ifdef spellings. A name inside defined() asks whether that name exists, so
  # it is not a call to it.
  function operands(keyword, cond,   n) {
    if (keyword == "ifdef" || keyword == "ifndef") { return "" }
    n = 1
    while (n > 0) { n = sub(/defined[ \t]*\([^()]*\)/, " ", cond) }
    n = 1
    while (n > 0) { n = sub(/defined[ \t]+[A-Za-z_][A-Za-z0-9_]*/, " ", cond) }
    return cond
  }

  function scan(isGated, haystack, display,   k) {
    if (isGated) { return }
    for (k = 1; k <= count; k++) {
      if (haystack ~ ("(^|[^A-Za-z0-9_])" name[k] "([^A-Za-z0-9_]|$)")) {
        printf "%s:%d:%s:%s\n", FILENAME, FNR, name[k], display
      }
    }
  }

  BEGIN {
    count = split(names, name, " ")
    APOS = sprintf("%c", 39)
    inblock = 0
    depth = 0
    scanned = 0
  }

  FNR == 1 { scanned++; inblock = 0; depth = 0 }

  {
    line = $0
    sub(/\r$/, "", line)
    code = stripped(line)

    if (code ~ /^[ \t]*#/) {
      rest = code
      sub(/^[ \t]*#[ \t]*/, "", rest)
      kw = rest
      sub(/[^A-Za-z_].*$/, "", kw)
      cond = rest
      sub(/^[A-Za-z_]*/, "", cond)

      if (kw == "if" || kw == "ifdef" || kw == "ifndef") {
        scan(gatedRange(1, depth), operands(kw, cond), code)
        depth++
        kind[depth] = guardKind(kw, cond)
        gated[depth] = (kind[depth] == "positive") ? 1 : 0
        next
      }
      if (kw == "elif" || kw == "else") {
        if (depth > 0) {
          if (kind[depth] == "positive") { gated[depth] = 0 }
          else if (kind[depth] == "negative") { gated[depth] = 1 }
          if (kw == "elif") {
            flipped = guardKind("if", cond)
            if (flipped != "other") {
              kind[depth] = flipped
              gated[depth] = (flipped == "positive") ? 1 : 0
            }
          }
        }
        scan(gatedRange(1, depth - 1), operands("if", cond), code)
        next
      }
      if (kw == "endif") {
        if (depth > 0) { depth-- }
        next
      }
    }

    scan(gatedRange(1, depth), code, code)
  }

  END { printf "#scanned:%d\n", scanned }
' "${FILES[@]}")

SCANNED=$(printf '%s\n' "$RAW" | sed -n 's/^#scanned:\(.*\)$/\1/p')
REPORT=$(printf '%s\n' "$RAW" | sed '/^#scanned:/d')

if [ -z "$SCANNED" ]; then
  echo "ERROR: the scan printed no file count, so what it read is unknown." >&2
  exit 1
fi

echo "    $SCANNED of $TOTAL_COUNT files under tests/ read, across $STUB_DIR_COUNT stub directories"

FAIL=0

UNREAD=$(printf '%s\n' "${ALL_FILES[@]}" | grep -Ev "$SOURCE_EXT_RE" || true)
if [ -n "$UNREAD" ]; then
  while IFS= read -r path; do
    case " $NOT_SOURCE " in
      *" ${path##*.} "*)
        echo "    not read, holds no code: $path"
        ;;
      *)
        echo "ERROR: nothing in $path was scanned. Its extension" >&2
        echo "       is in neither list at the top of this file, so this script did not" >&2
        echo "       read it. Add the extension to SOURCE_EXT_RE, or to NOT_SOURCE if the" >&2
        echo "       file holds no code." >&2
        FAIL=1
        ;;
    esac
  done <<< "$UNREAD"
fi

if [ "$SCANNED" -ne "${#FILES[@]}" ]; then
  echo "ERROR: ${#FILES[@]} files were handed to the scan and $SCANNED produced a line." >&2
  echo "       A file holding no lines reads here the same as a file with nothing in it" >&2
  echo "       to find." >&2
  FAIL=1
fi
if [ "$STUB_DIR_COUNT" -lt 7 ]; then
  echo "ERROR: only $STUB_DIR_COUNT stub directories were found and there were 7 on" >&2
  echo "       2026-11-08. Either stubs were removed, or the find above stopped matching" >&2
  echo "       and this script read less than tests/ holds." >&2
  FAIL=1
fi
if [ "$SCANNED" -lt 29 ]; then
  echo "ERROR: only $SCANNED files were read under tests/ and 29 were read on 2026-11-08." >&2
  echo "       Either tests were retired, or the extension list above stopped matching" >&2
  echo "       what they are named." >&2
  FAIL=1
fi

if [ -n "$REPORT" ]; then
  echo "$REPORT" | while IFS=: read -r file lineno interface code; do
    echo "ERROR: $file:$lineno reaches for $interface outside a block guarded on _WIN32." >&2
    echo "       $(printf '%s' "$code" | sed 's/^[ \t]*//')" >&2
    echo "       That is the line with comments and literals taken out, which is what" >&2
    echo "       the scan read. The name is in Microsoft's C runtime and not in glibc, so" >&2
    echo "       this file compiles here and does not compile on ubuntu-24.04. Put it" >&2
    echo "       behind '#ifdef _WIN32' with the POSIX call in the #else, the way" >&2
    echo "       tests/announcement-and-home-value-stubs/Arduino.h holds gmtime_r." >&2
  done
  FAIL=1
fi

if [ "$FAIL" -ne 0 ]; then
  exit 1
fi

echo "PASS: host test stubs use no Windows-only interface outside a block guarded on _WIN32"
