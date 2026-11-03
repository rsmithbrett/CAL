#!/usr/bin/env bash
# Fails when the Unreleased section of RELEASE_NOTES.md still holds an entry the
# previous release already published.
#
# build-and-release.yml composes the release body out of that section and
# nothing empties it, so an entry left behind goes out again under the next tag.
# v2026.10.18.0001 and v2026.10.22.0001 published prose that differed only in
# their checksum lines, and v2026.11.03.0004's two entries were still under
# Unreleased after it shipped.
#
# What it compares are the `### ` heading lines. The previous release's body is
# the record of what published, so a heading out of it still sitting under
# Unreleased means that entry was never moved under its dated heading. A heading
# names the change it describes, which is why two releases sharing one reads as
# the roll being missed rather than as a coincidence.
#
# `### Installing this` and `### Checksums` are generated into every body by the
# workflow rather than read out of the file, so they are skipped.
#
# An empty Unreleased section passes, because a build with nothing user-visible
# in it still has to publish. An empty previous body passes for the same reason:
# the release before this one carried no entries to leave behind.
#
# PREV_BODY_FILE holds the previous release's body. In a workflow:
#
#   gh release view --json body --jq .body > prev-release-body.md
#   PREV_BODY_FILE=prev-release-body.md bash ci/check-release-notes-rolled.sh
#
# It runs before the release is created, so `gh release view` with no tag names
# the release this one follows.
#
# By hand on a workstation where `gh` hangs, the body comes off the API at
# https://api.github.com/repos/rsmithbrett/CAL/releases/latest - the script reads
# the body as plain Markdown in a file, however that file was produced.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

if [ -z "${PREV_BODY_FILE:-}" ]; then
  echo "ERROR: set PREV_BODY_FILE to the previous release's body." >&2
  echo "  gh release view --json body --jq .body > prev-release-body.md" >&2
  exit 1
fi
if [ ! -f "$PREV_BODY_FILE" ]; then
  echo "ERROR: PREV_BODY_FILE names no file: $PREV_BODY_FILE" >&2
  exit 1
fi

echo "==> Shipped notes rolled under their own heading"

# The same extraction build-and-release.yml composes the body from, so what is
# read here is what would publish.
UNRELEASED=$(awk '/^## Unreleased$/{on=1; next} on && /^## /{exit} on' RELEASE_NOTES.md \
  | sed '/^---$/d')

# Carriage returns, because the previous body arrives over HTTP and the file is
# read off a filesystem that may hold either ending.
PREV_HEADINGS=$(tr -d '\r' < "$PREV_BODY_FILE" \
  | grep '^### ' \
  | grep -vx '### Installing this' \
  | grep -vx '### Checksums' \
  || true)

if [ -z "$PREV_HEADINGS" ]; then
  echo "The previous release published no entries, so there is nothing to have left behind."
  exit 0
fi

REPEATED=0
while IFS= read -r heading; do
  [ -n "$heading" ] || continue
  if printf '%s\n' "$UNRELEASED" | tr -d '\r' | grep -Fxq -- "$heading"; then
    echo "ERROR: this entry already published and is still under Unreleased:" >&2
    echo "  $heading" >&2
    REPEATED=$((REPEATED + 1))
  fi
done <<< "$PREV_HEADINGS"

if [ "$REPEATED" -ne 0 ]; then
  echo "ERROR: $REPEATED entry/entries would publish a second time." >&2
  echo "Move them under a dated heading for the release that carried them, the way" >&2
  echo "RELEASE_NOTES.md's own opening says, and leave Unreleased holding only what" >&2
  echo "this release adds." >&2
  exit 1
fi

PREV_COUNT=$(printf '%s\n' "$PREV_HEADINGS" | grep -c '^### ')
echo "PASS: release notes rolled, $PREV_COUNT shipped heading(s) checked against Unreleased"
