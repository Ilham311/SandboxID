#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
JNI="$ROOT/jni"

echo "==> Ternak TT — Path B lsplant fetcher"
echo "==> Root: $ROOT"

if ! command -v git >/dev/null 2>&1; then
    echo "!! git not found. Install git and re-run." >&2
    exit 1
fi

cd "$JNI"

if [ ! -d lsplant ]; then
    echo "==> Cloning LSPosed/LSPlant..."
    git clone --depth 1 --recurse-submodules https://github.com/LSPosed/LSPlant.git lsplant
else
    echo "==> lsplant already present"
fi

if [ ! -d dobby ]; then
    echo "==> Cloning jmpews/Dobby..."
    git clone --depth 1 --recurse-submodules https://github.com/jmpews/Dobby.git dobby
else
    echo "==> dobby already present"
fi

cd "$ROOT"

HAS_JAVAC=0; HAS_D8=0
command -v javac >/dev/null 2>&1 && HAS_JAVAC=1
command -v d8    >/dev/null 2>&1 && HAS_D8=1

if [ -f java_helper/TernakHookHelper.java ] && [ "$HAS_JAVAC" = "1" ] && [ "$HAS_D8" = "1" ]; then
    echo "==> Compiling TernakHookHelper.java -> classes.dex -> jni/helper_dex.h"
    TMP="$(mktemp -d)"
    javac -source 8 -target 8 -d "$TMP" java_helper/TernakHookHelper.java
    d8 --output "$TMP" "$TMP"/*.class
    if command -v xxd >/dev/null 2>&1; then
        xxd -i -n HELPER_DEX "$TMP/classes.dex" > "$JNI/helper_dex.h"
        echo 'static const size_t HELPER_DEX_LEN = HELPER_DEX_len;' >> "$JNI/helper_dex.h"
    fi
    rm -rf "$TMP"
    echo "==> Generated jni/helper_dex.h"
else
    echo "!! javac/d8 or java_helper/ missing — java_hooks Init() will fail-soft (module still works, Path B disabled)"
fi

echo ""
echo "==> Done. Now run: ./build.sh"
