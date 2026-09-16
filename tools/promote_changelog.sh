#!/usr/bin/env bash
# Promotes the "## Unreleased" section of CHANGELOG.md to a versioned section.
#
# Called by .github/workflows/build.yml at release time. The maintainer
# accumulates prose under ## Unreleased while working; cutting a release then
# needs no manual changelog edit, because this renames that heading to the
# release version and date and re-inserts a fresh empty ## Unreleased above it.
#
# A section with no body — just the heading and whitespace — is left alone, so
# a release with no curated notes does not create a hollow version heading.
#
# The transform validates before it writes: the rewritten file must contain the
# new version heading and must preserve the promoted section's line count, or
# the original is left untouched and the script exits non-zero. A release that
# cannot promote its changelog is a release worth stopping to look at.
#
# Usage:  NV=v2.2.8 ./tools/promote_changelog.sh   (from the repo root)
#         NV=v2.2.8 DRY_RUN=1 ./tools/promote_changelog.sh   # preview, no write
set -euo pipefail

NV="${NV:?promote_changelog.sh: NV (e.g. v2.2.8) is required}"
CHANGELOG="${CHANGELOG:-CHANGELOG.md}"
[ -f "$CHANGELOG" ] || { echo "promote_changelog.sh: $CHANGELOG not found; nothing to promote" >&2; exit 0; }

DATE="$(date -u +%Y-%m-%d)"

# Body of the Unreleased section: lines after its heading up to the next
# heading, ignoring blanks. Zero means an empty section.
BODY_N="$(awk '
  /^## Unreleased$/ { in_sec = 1; next }
  /^## / && in_sec  { in_sec = 0 }
  in_sec && NF      { n++ }
  END { print n + 0 }
' "$CHANGELOG")"

if [ "$BODY_N" -eq 0 ]; then
  echo "promote_changelog.sh: the Unreleased section is empty; leaving $CHANGELOG untouched"
  exit 0
fi

TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT

# One pass: insert a fresh Unreleased heading under the document title, and
# retitle the existing one. The inserted heading is emitted with `next`, so the
# retitle rule below can never match it.
awk -v ver="$NV" -v date="$DATE" '
  FNR == 1 && /^#/          { print; print ""; print "## Unreleased"; print ""; next }
  !done && /^## Unreleased$/ { print "## " ver " (" date ")"; done = 1; next }
                             { print }
' "$CHANGELOG" > "$TMP"

# Validate before swapping: the new heading must be present exactly once, and
# every body line of the old Unreleased section must have survived the rewrite.
if [ "$(grep -c "^## ${NV} (${DATE})$" "$TMP" || true)" -ne 1 ]; then
  echo "promote_changelog.sh: the rewritten file does not carry exactly one '## ${NV} (${DATE})' heading; not writing" >&2
  exit 1
fi
PROMOTED_N="$(awk -v ver="$NV" -v date="$DATE" '
  $1 == "##" && $2 == ver && $3 == "(" date ")" { in_sec = 1; next }
  $1 == "##" && in_sec                          { in_sec = 0 }
  in_sec && NF                                  { n++ }
  END { print n + 0 }
' "$TMP")"
if [ "$PROMOTED_N" -ne "$BODY_N" ]; then
  echo "promote_changelog.sh: promoted section has $PROMOTED_N content lines, expected $BODY_N; not writing" >&2
  exit 1
fi

if [ "${DRY_RUN:-0}" = "1" ]; then
  echo "promote_changelog.sh: dry run — would promote ${BODY_N} content lines to '## ${NV} (${DATE})'"
  cat "$TMP"
  exit 0
fi

cat "$TMP" > "$CHANGELOG"
echo "promote_changelog.sh: promoted ${BODY_N} content lines to '## ${NV} (${DATE})', fresh Unreleased inserted"
