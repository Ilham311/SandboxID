#!/usr/bin/env bash
# gen_hook_dex.sh — compile the LSPlant callback class to a DEX and embed it as
# a C header (jni/hook_dex.h) that jni/sbx_lsplant.hpp includes.
#
# WHY THIS EXISTS
#   The per-app ANDROID_ID hook works by asking LSPlant to route
#   Settings.Secure.getString() through a *Java* callback. That callback is
#   androidx.core.os.EnvCompatState (source: jni/SandboxIDHook.java). LSPlant
#   loads it at runtime from an in-memory DEX (InMemoryDexClassLoader), so the
#   compiled DEX must be embedded into libsandboxid.so as a byte array.
#
#   sbx_lsplant.hpp guards the whole callback path behind
#       #if __has_include("hook_dex.h")  ->  #define SBX_HAVE_HOOK_DEX
#   so when this header is absent the hook is *silently skipped* (that is the
#   "doubly disabled" state described in README.md). Running this script flips
#   SBX_HAVE_HOOK_DEX on for the next build.
#
# OUTPUT (symbol names are required verbatim by sbx_lsplant.hpp):
#   static const unsigned char hook_dex[];
#   static const unsigned int  hook_dex_len;
#
# REQUIREMENTS
#   - javac  : JDK 8+ (JAVA_HOME or on PATH)
#   - d8     : Android SDK build-tools >= 28 ($D8, or PATH, or under
#              $ANDROID_HOME/$ANDROID_SDK_ROOT build-tools/*)
#   - one of : python3 | xxd | od  (for the byte-array emit; all are common)
#
# Safe to re-run; it overwrites jni/hook_dex.h atomically.
set -euo pipefail

# --- locate paths -----------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
JNI_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
JAVA_SRC="$JNI_DIR/SandboxIDHook.java"
OUT_HEADER="$JNI_DIR/hook_dex.h"

if [ ! -f "$JAVA_SRC" ]; then
  echo "ERROR: source not found: $JAVA_SRC" >&2
  exit 1
fi

# --- locate javac -----------------------------------------------------------
JAVAC=""
if [ -n "${JAVA_HOME:-}" ] && [ -x "$JAVA_HOME/bin/javac" ]; then
  JAVAC="$JAVA_HOME/bin/javac"
elif command -v javac >/dev/null 2>&1; then
  JAVAC="$(command -v javac)"
else
  echo "ERROR: javac not found (install a JDK 8+ or set JAVA_HOME)" >&2
  exit 1
fi

# --- locate d8 --------------------------------------------------------------
# Search order: $D8, PATH, then the highest-versioned build-tools that ships d8.
D8_BIN="${D8:-}"
if [ -z "$D8_BIN" ] && command -v d8 >/dev/null 2>&1; then
  D8_BIN="$(command -v d8)"
fi
if [ -z "$D8_BIN" ]; then
  for root in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}"; do
    if [ -z "$root" ] || [ ! -d "$root/build-tools" ]; then continue; fi
    while IFS= read -r cand; do
      if [ -x "$cand" ]; then D8_BIN="$cand"; fi   # last (highest version) wins
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

# --- optional android.jar for d8 --lib (desugaring bootclasspath) -----------
# EnvCompatState uses only java.lang.* / java.lang.reflect.* (no android APIs,
# no lambdas/default methods) so desugaring is a no-op and --lib is optional;
# we pass it when available purely to silence d8's advisory.
ANDROID_JAR=""
for root in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}"; do
  if [ -z "$root" ] || [ ! -d "$root/platforms" ]; then continue; fi
  while IFS= read -r cand; do
    if [ -f "$cand" ]; then ANDROID_JAR="$cand"; fi
  done < <(find "$root/platforms" -maxdepth 2 -name android.jar 2>/dev/null | sort -V)
  if [ -n "$ANDROID_JAR" ]; then break; fi
done

# --- scratch dir ------------------------------------------------------------
WORK="$(mktemp -d)"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT
mkdir -p "$WORK/classes" "$WORK/dex"

echo "==> javac : $JAVAC"
echo "==> d8    : $D8_BIN"
if [ -n "$ANDROID_JAR" ]; then echo "==> lib   : $ANDROID_JAR"; fi

# --- compile to Java 8 bytecode (widest dex compatibility) ------------------
if ! "$JAVAC" --release 8 -d "$WORK/classes" "$JAVA_SRC" 2>"$WORK/javac.log"; then
  # Fall back for JDKs that reject --release (very old) — use -source/-target.
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

# --- dex --------------------------------------------------------------------
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

# --- embed classes.dex as a C byte array -----------------------------------
TMP_HEADER="$WORK/hook_dex.h"
DEX_LEN="$(wc -c < "$DEX" | tr -d ' ')"
{
  echo "// Auto-generated by jni/tools/gen_hook_dex.sh — DO NOT EDIT."
  echo "// Source : jni/SandboxIDHook.java  (class androidx.core.os.EnvCompatState)"
  echo "// Consumer: jni/sbx_lsplant.hpp (loaded via InMemoryDexClassLoader at runtime)."
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
  # xxd -i from stdin emits only the "0x..," rows (no declaration wrapper).
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
