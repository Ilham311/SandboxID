#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
JNI_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
JAVA_SRC="$JNI_DIR/SandboxIDHook.java"
OUT_HEADER="$JNI_DIR/hook_dex.h"

if [ ! -f "$JAVA_SRC" ]; then
  echo "ERROR: source not found: $JAVA_SRC" >&2
  exit 1
fi

JAVAC=""
if [ -n "${JAVA_HOME:-}" ] && [ -x "$JAVA_HOME/bin/javac" ]; then
  JAVAC="$JAVA_HOME/bin/javac"
elif command -v javac >/dev/null 2>&1; then
  JAVAC="$(command -v javac)"
else
  echo "ERROR: javac not found (install a JDK 8+ or set JAVA_HOME)" >&2
  exit 1
fi

D8_BIN="${D8:-}"
if [ -z "$D8_BIN" ] && command -v d8 >/dev/null 2>&1; then
  D8_BIN="$(command -v d8)"
fi
if [ -z "$D8_BIN" ]; then
  for root in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}"; do
    if [ -z "$root" ] || [ ! -d "$root/build-tools" ]; then continue; fi
    while IFS= read -r cand; do
      if [ -x "$cand" ]; then D8_BIN="$cand"; fi
    done < <(find "$root/build-tools" -maxdepth 2 -name d8 -type f 2>/dev/null | sort -V)
    if [ -n "$D8_BIN" ]; then break; fi
  done
fi
if [ -z "$D8_BIN" ]; then
  {
    echo "ERROR: d8 not found."
    echo "  Install Android SDK build-tools (>=28) and set ANDROID_HOME or"
    echo "  ANDROID_SDK_ROOT, or put d8 on PATH, or set \$D8 to its full path."
  } >&2
  exit 1
fi

ANDROID_JAR=""
for root in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}"; do
  if [ -z "$root" ] || [ ! -d "$root/platforms" ]; then continue; fi
  while IFS= read -r cand; do
    if [ -f "$cand" ]; then ANDROID_JAR="$cand"; fi
  done < <(find "$root/platforms" -maxdepth 2 -name android.jar 2>/dev/null | sort -V)
  if [ -n "$ANDROID_JAR" ]; then break; fi
done

WORK="$(mktemp -d)"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT
mkdir -p "$WORK/classes" "$WORK/dex"

echo "==> javac : $JAVAC"
echo "==> d8    : $D8_BIN"
if [ -n "$ANDROID_JAR" ]; then echo "==> lib   : $ANDROID_JAR"; fi

if ! "$JAVAC" --release 8 -d "$WORK/classes" "$JAVA_SRC" 2>"$WORK/javac.log"; then
  if ! "$JAVAC" -source 1.8 -target 1.8 -d "$WORK/classes" "$JAVA_SRC" 2>>"$WORK/javac.log"; then
    echo "ERROR: javac failed:" >&2
    cat "$WORK/javac.log" >&2
    exit 1
  fi
fi

CLASSES=()
while IFS= read -r c; do CLASSES+=("$c"); done \
  < <(find "$WORK/classes" -name '*.class' | sort)
if [ "${#CLASSES[@]}" -eq 0 ]; then
  echo "ERROR: javac produced no .class files" >&2
  exit 1
fi

D8_ARGS=(--min-api 26 --output "$WORK/dex")
if [ -n "$ANDROID_JAR" ]; then D8_ARGS+=(--lib "$ANDROID_JAR"); fi
if ! "$D8_BIN" "${D8_ARGS[@]}" "${CLASSES[@]}" 2>"$WORK/d8.log"; then
  echo "ERROR: d8 failed:" >&2
  cat "$WORK/d8.log" >&2
  exit 1
fi

DEX="$WORK/dex/classes.dex"
if [ ! -f "$DEX" ]; then
  echo "ERROR: d8 did not produce classes.dex" >&2
  exit 1
fi

TMP_HEADER="$WORK/hook_dex.h"
DEX_LEN="$(wc -c < "$DEX" | tr -d ' ')"
{
  echo "#pragma once"
  echo "static const unsigned char hook_dex[] = {"
} > "$TMP_HEADER"

if command -v python3 >/dev/null 2>&1; then
  python3 - "$DEX" >> "$TMP_HEADER" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
for i in range(0, len(data), 12):
    sys.stdout.write("    " + ", ".join("0x%02x" % b for b in data[i:i + 12]) + ",\n")
PY
elif command -v xxd >/dev/null 2>&1; then
  xxd -i < "$DEX" >> "$TMP_HEADER"
else
  od -An -v -tu1 "$DEX" | awk '{ for (i = 1; i <= NF; i++) printf "    0x%02x,\n", $i }' >> "$TMP_HEADER"
fi

{
  echo "};"
  echo "static const unsigned int hook_dex_len = ${DEX_LEN}u;"
} >> "$TMP_HEADER"

mv -f "$TMP_HEADER" "$OUT_HEADER"
echo "==> wrote $OUT_HEADER (${DEX_LEN} bytes of DEX embedded)"
