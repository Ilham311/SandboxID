#!/usr/bin/env bash
# Generates the GitHub release notes for one SandboxID release.
#
# Called by .github/workflows/build.yml. Everything it needs arrives as
# environment, so it also runs locally from a checkout to preview what the
# next release is going to say:
#
#   REPO=Ilham311/SandboxID NV=v2.2.8 NC=239 PREV=v2.2.7 \
#     OUT=/tmp/notes.md ./tools/gen_release_notes.sh
#
# The notes are derived from the repository rather than written per release, so
# a release never ships with a thin or stale body just because nobody composed
# one. Anything worth saying in the maintainer's own words belongs in
# CHANGELOG.md, whose section for this version is quoted verbatim under
# "What's new".

set -euo pipefail

NV="${NV:?gen_release_notes.sh: NV (e.g. v2.2.8) is required}"
NC="${NC:?gen_release_notes.sh: NC (e.g. 239) is required}"
REPO="${REPO:?gen_release_notes.sh: REPO (owner/name, e.g. Ilham311/SandboxID) is required}"
OUT="${OUT:-release_notes.md}"
PREV="${PREV:-}"
DIST="${DIST:-dist}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# ---- what the build actually ships, parsed out of build.sh -----------------
# Parsed rather than restated, so the notes cannot drift from the build: an ABI
# added to build.sh shows up here without a second edit.
ABIS="$(sed -n 's/^ABIS=(\(.*\))$/\1/p' build.sh | head -n 1)"
[ -n "$ABIS" ] || ABIS="arm64-v8a armeabi-v7a x86_64 x86"
ABIS_DISPLAY="${ABIS// /, }"
MIN_SDK="$(sed -n 's/^MIN_SDK=.*:-\([0-9][0-9]*\)}.*$/\1/p' build.sh | head -n 1)"
[ -n "$MIN_SDK" ] || MIN_SDK=26

# sha256 of a packaged zip. Empty (not an error) when run before a build, so a
# local preview still works; the release always has the checksums file.
sha_of() {
  [ -f "$DIST/checksums.sha256" ] || return 0
  awk -v want="$1" '$2 == want { print $1; exit }' "$DIST/checksums.sha256"
}
size_of() { du -h "$DIST/$1" 2>/dev/null | cut -f1 || true; }
cell()    { [ -n "$1" ] && printf '%s' "$1" || printf -- '-'; }

# ---- the commit range these notes cover ------------------------------------
# Without a previous tag, fall back to the newest 20 commits so a first release
# still says something concrete instead of listing the whole history.
if [ -n "$PREV" ] && git rev-parse -q --verify "refs/tags/${PREV}" >/dev/null 2>&1; then
  RANGE="${PREV}..HEAD"
else
  PREV=""
  OLDEST="$(git rev-list --max-count=20 --reverse HEAD 2>/dev/null | head -n 1)"
  RANGE="${OLDEST:-HEAD}..HEAD"
fi

COMMITS_N="$(git rev-list --count "$RANGE" 2>/dev/null || echo 0)"
STATS="$(git diff --shortstat "$RANGE" 2>/dev/null | sed 's/^ //')"

# ---- maintainer prose for this version, if CHANGELOG.md has it -------------
# Field comparison instead of a regex, so "## v2.2.8" and "## v2.2.8 (date)"
# both match and no other heading can. The section ends at the next heading of
# any kind, so a trailing "## Unreleased" can never bleed in.
CL_SECTION=""
if [ -f CHANGELOG.md ]; then
  CL_SECTION="$(awk -v ver="$NV" '
    $1 == "##" && $2 == ver { flag = 1; next }
    $1 == "##" && flag     { exit }
    flag                   { print }
  ' CHANGELOG.md)"
fi

# ---- the commit list, grouped by conventional-commit type ------------------
# Subject<TAB>short-hash. The machine-generated release-sync commits are
# dropped: they carry nothing a reader does not already have, and in a quiet
# range they would otherwise be the entire list.
RAW="$(git log --format='%s%x09%h' "$RANGE" 2>/dev/null || true)"
RAW="$(printf '%s\n' "$RAW" | grep -vE '^chore\(release\): sync[[:space:]]' || true)"

GROUPED="$(printf '%s\n' "$RAW" | awk -F'\t' '
  function grp(s) {
    if (s ~ /^(feat|feature)/) return "feat";
    if (s ~ /^fix/)            return "fix";
    if (s ~ /^perf/)           return "perf";
    if (s ~ /^refactor/)       return "refactor";
    if (s ~ /^tests?/)         return "test";
    if (s ~ /^docs?/)          return "docs";
    if (s ~ /^(build|ci)/)     return "build";
    if (s ~ /^style/)          return "style";
    if (s ~ /^revert/)         return "revert";
    if (s ~ /^chore/)          return "chore";
    return "other";
  }
  NF { print grp($1) "\t" "- " $1 " (" $2 ")" }
')"

# Breaking changes lead the notes whatever their type — they are the one thing
# a reader must not skim past. Conventional commits mark them either with a `!`
# before the colon, or with a `BREAKING CHANGE:` footer in the message body.
BRK_FILE="$(mktemp)"
GROUP_FILE="$(mktemp)"
trap 'rm -f "$BRK_FILE" "$GROUP_FILE"' EXIT
{
  git log --format='%B' "$RANGE" 2>/dev/null \
    | grep -iE '^[[:space:]]*BREAKING[[:space:]]+CHANGE' || true
  printf '%s\n' "$RAW" \
    | awk -F'\t' '$1 ~ /^[a-z]+([(].*[)])?!:/ { print "- " $1 " (" $2 ")" }' || true
} > "$BRK_FILE"

emit_group() {
  local key="$1" icon="$2" label="$3" body
  body="$(printf '%s\n' "$GROUPED" | awk -F'\t' -v g="$key" '$1 == g { print $2 }')"
  [ -z "$body" ] && return 0
  printf '### %s %s\n\n' "$icon" "$label"
  printf '%s\n' "$body"
  printf '\n'
}

{
  emit_group feat     "✨" "Features"
  emit_group fix      "🐛" "Fixes"
  emit_group perf     "⚡" "Performance"
  emit_group refactor "♻️" "Refactor"
  emit_group test     "🧪" "Tests"
  emit_group docs     "📚" "Docs"
  emit_group build    "📦" "Build & CI"
  emit_group style    "🎨" "Style"
  emit_group revert   "↩️" "Reverts"
  emit_group chore    "🧹" "Chore"
  emit_group other    "🧩" "Other"
} > "$GROUP_FILE"

# Cap the rendered list: a large range is a wall of text nobody scrolls. Cut at
# a section boundary so no heading is left dangling, then link the remainder.
MAX_BULLETS=80
SHOWN_BULLETS="$(grep -c '^- ' "$GROUP_FILE" || true)"
if [ "${SHOWN_BULLETS:-0}" -gt "$MAX_BULLETS" ]; then
  HIDDEN=$((SHOWN_BULLETS - MAX_BULLETS))
  if [ -n "$PREV" ]; then
    COMPARE="https://github.com/${REPO}/compare/${PREV}...${NV}"
  else
    COMPARE="https://github.com/${REPO}/commits/main"
  fi
  awk -v max="$MAX_BULLETS" '
    /^- / && shown >= max { exit }
    /^- /                { shown++ }
    { print }
  ' "$GROUP_FILE" > "$GROUP_FILE.cut"
  printf '\n_… and %d more commits — [full comparison](%s)_\n' "$HIDDEN" "$COMPARE" \
    >> "$GROUP_FILE.cut"
  mv "$GROUP_FILE.cut" "$GROUP_FILE"
fi

# ---- metadata --------------------------------------------------------------
SHA_FULL="${GITHUB_SHA:-}"
[ -n "$SHA_FULL" ] || SHA_FULL="$(git rev-parse HEAD 2>/dev/null || echo '')"
SHA_SHORT="${SHA_FULL:0:7}"
[ -n "$SHA_SHORT" ] || SHA_SHORT="HEAD"

if [ -n "$PREV" ]; then
  RANGE_LINE="${COMMITS_N} commits since \`${PREV}\`"
else
  RANGE_LINE="the newest ${COMMITS_N} commits"
fi

# Authors over the same filtered set the list above displays, so a release
# never thanks the release bot for its own sync commits.
AUTHORS="$(git log --format='%an%x09%s' "$RANGE" 2>/dev/null \
             | grep -vE $'\tchore\(release\): sync[[:space:]]' \
             | cut -f1 | sort -u | grep -v '^$' || true)"
AUTHORS_DISPLAY="$(printf '%s\n' "$AUTHORS" | paste -sd, - | sed 's/,/, /g')"

SHA_REL="$(sha_of "sandboxid-${NV}-release.zip")"
SHA_DBG="$(sha_of "sandboxid-${NV}-debug.zip")"
SZ_REL="$(size_of "sandboxid-${NV}-release.zip")"
SZ_DBG="$(size_of "sandboxid-${NV}-debug.zip")"

# ---- assemble the document -------------------------------------------------
{
  echo "# SandboxID ${NV}"
  echo
  echo "| | |"
  echo "|---|---|"
  echo "| **Build** | \`$(date -u '+%Y-%m-%d %H:%M:%S UTC')\` |"
  echo "| **Commit** | [\`${SHA_SHORT}\`](https://github.com/${REPO}/commit/${SHA_FULL}) |"
  echo "| **versionCode** | ${NC} |"
  echo "| **Android** | API ${MIN_SDK}+ |"
  echo "| **ABIs** | ${ABIS_DISPLAY} |"
  echo "| **Range** | ${RANGE_LINE} |"
  if [ -n "$STATS" ]; then
    echo "| **Diff** | ${STATS} |"
  fi
  if [ -n "$AUTHORS_DISPLAY" ]; then
    echo "| **Contributors** | ${AUTHORS_DISPLAY} |"
  fi
  echo

  if [ -n "$CL_SECTION" ]; then
    echo "## What's new"
    echo
    printf '%s\n' "$CL_SECTION"
    echo
  fi

  echo "## Downloads"
  echo
  echo "**Which one?** \`release\` is what to run day to day. \`debug\` adds verbose"
  echo "LOGD and on-device log capture for troubleshooting — it is not faster and"
  echo "not better, just louder. Most people want the release zip."
  echo
  echo "| File | Size | SHA-256 |"
  echo "|------|------|---------|"
  printf '| [`sandboxid-%s-release.zip`](%s) | %s | `%s` |\n' \
    "$NV" "https://github.com/${REPO}/releases/download/${NV}/sandboxid-${NV}-release.zip" \
    "$(cell "$SZ_REL")" "$(cell "$SHA_REL")"
  printf '| [`sandboxid-%s-debug.zip`](%s) | %s | `%s` |\n' \
    "$NV" "https://github.com/${REPO}/releases/download/${NV}/sandboxid-${NV}-debug.zip" \
    "$(cell "$SZ_DBG")" "$(cell "$SHA_DBG")"
  echo
  # This release is the first whose debug package reads its own update channel.
  # A device already on an older debug build is still on the release channel and
  # would flip on update — say so, and point at the fix.
  echo "_Already running a \`debug\` build from before this version? Install this"
  echo "release's \`debug\` zip by hand once: from here on a debug install updates"
  echo "to the next debug build instead of silently becoming a release build._"
  echo

  echo "## Install"
  echo
  echo "1. Download the zip for your variant (release, unless you are troubleshooting)"
  echo "2. Magisk / KernelSU / APatch → Modules → Install from storage"
  echo "3. Reboot"
  echo "4. Tap the module's **Action** button to rotate the persona (it auto-locks after)"
  echo
  echo "Nothing is modified by default: \`target.txt\` ships empty, so no app is"
  echo "touched until you add it there yourself."
  echo

  echo "## Verify the download"
  echo
  echo '```bash'
  echo "sha256sum sandboxid-${NV}-release.zip"
  echo '```'
  echo "Expect the hash in the table above. The complete checksum list for this"
  echo "release is at the bottom of these notes."
  echo

  if [ -s "$GROUP_FILE" ] || [ -s "$BRK_FILE" ]; then
    if [ -n "$PREV" ]; then
      echo "## Commits since \`${PREV}\`"
    else
      echo "## Recent commits"
    fi
    echo
    if [ -s "$BRK_FILE" ]; then
      echo "### ⚠️ Breaking changes"
      echo
      cat "$BRK_FILE"
      echo
    fi
    cat "$GROUP_FILE"
  fi

  echo "## SHA-256"
  echo
  echo '```'
  if [ -f "$DIST/checksums.sha256" ]; then
    cat "$DIST/checksums.sha256"
  else
    echo "(generated by the release build — run ./build.sh to produce them)"
  fi
  echo '```'
} > "$OUT"

echo "gen_release_notes.sh: wrote $OUT ($(wc -l < "$OUT" | tr -d ' ') lines)"
