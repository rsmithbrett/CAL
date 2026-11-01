#!/usr/bin/env bash
# Holds every draw function in App/Display.cpp to returning the panel's default
# text scale.
#
# LovyanGFX keeps a selected face, datum and scale until something sets them
# again, so whatever a card leaves behind is what the next, unrelated screen
# draws in. App/Display.cpp handles the face and the datum through
# restoreDefaultTextState(), called at the end of every card function. The scale
# had no such half: showStatus() set 2 for its headline and put 1 back only
# inside `if (detail.length() > 0)`, and four App.ino call sites pass that
# argument empty. TEXT_LAYOUT_DESIGN.md section 14 has the whole shape.
#
# Two rules are checked here, and together they are less than a control-flow
# model. Each one is named with what it does and does not reach.
#
# Rule 1, the restore is not inside a branch:
#
#   A function that selects a text scale other than 1 must put 1 back - by
#   `lcd.setTextSize(1);` or by `restoreDefaultTextState();` - at the
#   function's own two-space indentation, after the last place it set the other
#   scale.
#
# The indentation is the check. A restore nested in a branch is a restore on one
# path out, which is exactly what showStatus() had and exactly what reached the
# sports board.
#
# Rule 2, no early return while a scale is selected:
#
#   Reading a function top to bottom, no `return` may stand between a line that
#   selects a scale other than 1 and the next line that puts 1 back.
#
# Rule 1 alone passes a function that sets 2, returns early, and restores at the
# bottom: it compares two line numbers and the restore's number is the larger
# one. That shape is a path out with scale 2 selected, and it is the shape the
# document's own wording forbids, so rule 2 is what earns the wording.
#
# What neither rule reaches, because this is a linear read and not a graph: a
# restore inside a branch clears rule 2's flag for every return after it, so
#
#   lcd.setTextSize(2);
#   if (a) { restoreDefaultTextState(); }
#   if (b) return;                        // reachable with 2 selected when !a
#   lcd.setTextSize(1);
#
# passes both. Modeling that needs the branch structure, which an awk over a
# 6,700-line file is the wrong tool for. TEXT_LAYOUT_DESIGN.md section 14d says
# so rather than leaving it to be discovered.
#
# Deliberately a grep and two awks, in the same spirit as the two text gates in
# build-firmware.sh: it does not know what the panel looks like, it knows the
# difference between a function that put the scale back and one that did not.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

TARGET=App/Display.cpp

# The restore itself. Gutting this would pass every check below while putting
# nothing back, so it is asserted by name rather than assumed.
if ! awk '
  /^void restoreDefaultTextState\(\) \{/ { inside = 1; next }
  inside && /^\}/                        { exit found ? 0 : 1 }
  inside && /lcd\.setTextSize\(1\);/     { found = 1 }
  END                                    { exit found ? 0 : 1 }
' "$TARGET"; then
  echo "ERROR: restoreDefaultTextState() in $TARGET does not set a text scale of 1." >&2
  echo "       It is what the thirty call sites below it rely on to put the panel's" >&2
  echo "       text state back. See TEXT_LAYOUT_DESIGN.md section 14b." >&2
  exit 1
fi

# --- rule 1: the restore is not inside a branch -------------------------------
LEAKS=$(awk '
  function closeUnit() {
    if (scaled > 0 && restored < scaled) {
      printf "%s:%d: %s\n", FILENAME, scaled, signature
    }
    open = 0
  }
  # A unit runs from a line that starts a top-level definition to the line that
  # closes it. Comments, preprocessor lines, namespaces and type definitions are
  # not units: their members carry their own indentation and are read as part of
  # whatever unit encloses them.
  !open && /^[A-Za-z_~]/ && !/^(namespace|struct|class|enum|union|using|typedef|template|extern|constexpr|const |static |inline )/ {
    open = 1; signature = $0; scaled = 0; restored = 0; opener = FNR
  }
  open {
    if (/lcd\.setTextSize\(/ && !/lcd\.setTextSize\(1\)/) { scaled = FNR }
    if (/^  lcd\.setTextSize\(1\);/ || /^  restoreDefaultTextState\(\);/) { restored = FNR }
    # A definition written on one line closes itself. Everything else closes on
    # a brace in the first column.
    if (/^\}/ || (FNR == opener && /\{/ && /\}[ \t]*$/)) { closeUnit() }
  }
  END { if (open) closeUnit() }
' "$TARGET" || true)

if [ -n "$LEAKS" ]; then
  echo "ERROR: a function in $TARGET selects a text scale other than 1 and does not" >&2
  echo "       put 1 back at its own indentation, so the next screen drawn inherits it." >&2
  echo "$LEAKS" >&2
  echo "       Add 'lcd.setTextSize(1);' or 'restoreDefaultTextState();' before the" >&2
  echo "       function returns, outside any branch. A restore inside an 'if' covers one" >&2
  echo "       path out: that is what put a scale of 2 onto the sports board." >&2
  echo "       See TEXT_LAYOUT_DESIGN.md section 14." >&2
  exit 1
fi

# --- rule 2: no early return while a scale is selected ------------------------
#
# The same unit model as rule 1, read top to bottom with a flag instead of two
# line numbers. A comment is stripped before the line is read, because a
# sentence with the word "return" in it is not a path out; the strip wants
# whitespace or the start of the line before the slashes, so a "://" in a string
# is left alone.
EXITS=$(awk '
  !open && /^[A-Za-z_~]/ && !/^(namespace|struct|class|enum|union|using|typedef|template|extern|constexpr|const |static |inline )/ {
    open = 1; signature = $0; opener = FNR; dirty = 0; dirtyLine = 0
  }
  open {
    line = $0
    sub(/(^|[ \t])\/\/.*$/, "", line)
    if (line ~ /lcd\.setTextSize\(/ && line !~ /lcd\.setTextSize\(1\)/) {
      dirty = 1; dirtyLine = FNR
    } else if (line ~ /lcd\.setTextSize\(1\);/ || line ~ /restoreDefaultTextState\(\);/) {
      dirty = 0
    } else if (dirty && line ~ /(^|[^A-Za-z_0-9])return([ \t;(]|$)/) {
      printf "%s:%d: returns with the scale set at line %d still selected - %s\n", \
             FILENAME, FNR, dirtyLine, signature
    }
    if (line ~ /^\}/ || (FNR == opener && line ~ /\{/ && line ~ /\}[ \t]*$/)) { open = 0 }
  }
' "$TARGET" || true)

if [ -n "$EXITS" ]; then
  echo "ERROR: a function in $TARGET returns while a text scale other than 1 is" >&2
  echo "       selected, so the next screen drawn inherits it on that path." >&2
  echo "$EXITS" >&2
  echo "       Put 1 back before the return, or move the return after the restore." >&2
  echo "       A restore at the bottom of the function is not reached by a return" >&2
  echo "       above it, and rule 1 cannot see the difference." >&2
  echo "       See TEXT_LAYOUT_DESIGN.md section 14c." >&2
  exit 1
fi

echo "==> App/Display.cpp returns the default text scale from every draw function"
