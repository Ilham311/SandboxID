#!/usr/bin/env bash
set -euo pipefail

SRC="${1:-.}"
OUT="${2:-dist-public/ternak-tt-public.zip}"

log() { printf '[public] %s\n' "$*"; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

log "stage 1: copy source to $TMP"
cp -a "$SRC/." "$TMP/"
cd "$TMP"

log "stage 2: strip dev/source/docs files"
rm -rf .github .git .githooks jni prebuilt dist dist-public
rm -f .gitignore .gitattributes .editorconfig
rm -f README.md README_*.md CHANGELOG.md CONTRIBUTING.md CODE_OF_CONDUCT.md SECURITY.md
rm -f LICENSE LICENSE.md LICENSE.txt
rm -f build.sh build-public.sh strip_comments.py
rm -f update.json target.txt.example *.example
rm -f Makefile makefile Dockerfile

log "stage 3: install npm obfuscators (idempotent)"
for pkg in javascript-obfuscator clean-css-cli html-minifier-terser; do
  if ! command -v "$pkg" >/dev/null 2>&1; then
    npm install -g "$pkg" >/dev/null 2>&1 || true
  fi
done

log "stage 4: obfuscate webroot JS"
if [ -d webroot ]; then
  for js in webroot/*.js; do
    [ -f "$js" ] || continue
    log "  JS: $js"
    javascript-obfuscator "$js" --output "$js" \
      --compact true \
      --control-flow-flattening true \
      --control-flow-flattening-threshold 0.85 \
      --dead-code-injection true \
      --dead-code-injection-threshold 0.4 \
      --string-array true \
      --string-array-encoding 'base64' \
      --string-array-threshold 0.9 \
      --identifier-names-generator 'mangled-shuffled' \
      --rename-globals false \
      --self-defending true \
      --transform-object-keys true \
      --unicode-escape-sequence false \
      --disable-console-output true \
      --debug-protection false >/dev/null
  done

  log "stage 5: minify CSS"
  for css in webroot/*.css; do
    [ -f "$css" ] || continue
    log "  CSS: $css"
    cleancss -o "$css" "$css"
  done

  log "stage 6: minify HTML"
  for html in webroot/*.html; do
    [ -f "$html" ] || continue
    log "  HTML: $html"
    html-minifier-terser \
      --collapse-whitespace \
      --remove-comments \
      --remove-optional-tags \
      --minify-css true \
      --minify-js false \
      -o "$html" "$html"
  done
fi

log "stage 7: obfuscate shell scripts (base64+gzip pack)"
for sh in action.sh helpers.sh rotate_ids.sh summarize.sh customize.sh post-fs-data.sh service.sh; do
  [ -f "$sh" ] || continue
  log "  SH: $sh"
  SHEBANG="$(head -n1 "$sh")"
  case "$SHEBANG" in
    '#!'*) BODY_SKIP=1 ;;
    *)     SHEBANG='#!/system/bin/sh'; BODY_SKIP=0 ;;
  esac
  if [ "$BODY_SKIP" = "1" ]; then
    BLOB="$(tail -n +2 "$sh" | gzip -9c | base64 -w0)"
  else
    BLOB="$(cat "$sh" | gzip -9c | base64 -w0)"
  fi
  {
    echo "$SHEBANG"
    echo "_TT_PUB_D='$BLOB'"
    echo 'eval "$(printf %s "$_TT_PUB_D" | base64 -d 2>/dev/null | gzip -d 2>/dev/null)"'
  } > "$sh"
done

log "stage 8: rewrite module.prop (no updateJson, minimal description)"
python3 - <<'PY'
import re
p = 'module.prop'
lines = open(p).read().splitlines()
out = []
for ln in lines:
    if ln.startswith('updateJson='):
        continue
    if ln.startswith('description='):
        out.append('description=Zygisk device identity module (public build). Manual install only, no auto-update.')
        continue
    out.append(ln)
open(p, 'w').write('\n'.join(out) + '\n')
PY
cat module.prop

log "stage 9: strip any remaining stray comments in .prop / .json / .txt"
python3 - <<'PY'
import os
for root, dirs, files in os.walk('.'):
    dirs[:] = [d for d in dirs if d not in ('.git', 'node_modules')]
    for f in files:
        if not f.endswith(('.prop', '.json', '.txt')): continue
        p = os.path.join(root, f)
        s = open(p).read()
        out = []
        for ln in s.split('\n'):
            if ln.lstrip().startswith('#') and not ln.startswith('#!'): continue
            out.append(ln)
        open(p, 'w').write('\n'.join(out))
PY

log "stage 10: final tree"
find . -type f | sort

log "stage 11: zip"
OUT_ABS="$(cd "$(dirname "$SRC")" && pwd)/${OUT}"
OUT_ABS="$OUT"
case "$OUT" in
  /*) : ;;
  *)  OUT_ABS="$OLDPWD/$OUT" ;;
esac
mkdir -p "$(dirname "$OUT_ABS")"
rm -f "$OUT_ABS"
zip -qr "$OUT_ABS" . -x '*.zip'
log "done -> $OUT_ABS"
ls -lh "$OUT_ABS"
