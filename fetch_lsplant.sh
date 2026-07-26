#!/usr/bin/env bash
# Ternak TT — Path B fetcher (v1.1.6, AAR-based)
#
# Strategy:
#   1. Download lsplant-standalone AAR from Maven Central. Extract prefab .so
#      per ABI + lsplant.hpp header into prebuilt/lsplant/.
#      "standalone" = statically-linked libc++ (no libc++_shared.so dep).
#      This eliminates the v1.1.5-era pain of cloning LSPlant from source with
#      its SSH-URL test/ submodules.
#   2. git clone jmpews/Dobby (small clean repo, no submodule issues) into
#      jni/dobby/. LSPlant needs user-supplied inline_hooker, which we build
#      from Dobby.
#   3. javac + d8 compile java_helper/TernakHookHelper.java to jni/helper_dex.h.
#
# Version pinning:
#   LSPLANT_VERSION env var overrides. Default: auto-detect latest from Maven
#   metadata, fallback to hardcoded LSPLANT_FALLBACK_VERSION.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
JNI="$ROOT/jni"
PREBUILT="$ROOT/prebuilt/lsplant"

LSPLANT_FALLBACK_VERSION="6.4"
LSPLANT_MAVEN_GROUP="org/lsposed/lsplant"
LSPLANT_ARTIFACT="lsplant-standalone"
MAVEN_BASE="https://repo1.maven.org/maven2"

echo "==> Ternak TT — Path B lsplant fetcher (v1.1.6, AAR-based)"
echo "==> Root: $ROOT"

for cmd in git curl unzip; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "!! $cmd not found. Install $cmd and re-run." >&2
        exit 1
    fi
done

mkdir -p "$JNI" "$PREBUILT/include" "$PREBUILT/lib"

# ============================================================================
# 1. Resolve LSPlant version
# ============================================================================

resolve_lsplant_version() {
    if [ -n "${LSPLANT_VERSION:-}" ]; then
        echo "$LSPLANT_VERSION"
        return 0
    fi
    local meta_url="${MAVEN_BASE}/${LSPLANT_MAVEN_GROUP}/${LSPLANT_ARTIFACT}/maven-metadata.xml"
    local meta_xml
    meta_xml=$(curl -fsSL --max-time 30 "$meta_url" 2>/dev/null || true)
    if [ -n "$meta_xml" ]; then
        local latest
        latest=$(echo "$meta_xml" | sed -n 's:.*<release>\(.*\)</release>.*:\1:p' | head -1)
        if [ -z "$latest" ]; then
            latest=$(echo "$meta_xml" | sed -n 's:.*<latest>\(.*\)</latest>.*:\1:p' | head -1)
        fi
        if [ -n "$latest" ]; then
            echo "$latest"
            return 0
        fi
    fi
    echo "$LSPLANT_FALLBACK_VERSION"
}

LSPLANT_VER=$(resolve_lsplant_version)
echo "==> LSPlant version: $LSPLANT_VER"

# ============================================================================
# 2. Fetch + extract lsplant-standalone AAR
# ============================================================================

fetch_lsplant_aar() {
    local ver="$1"
    local aar_url="${MAVEN_BASE}/${LSPLANT_MAVEN_GROUP}/${LSPLANT_ARTIFACT}/${ver}/${LSPLANT_ARTIFACT}-${ver}.aar"
    local tmp_aar tmp_dir
    tmp_aar=$(mktemp -t lsplant.XXXXXX.aar)
    tmp_dir=$(mktemp -d -t lsplant-aar.XXXXXX)

    echo "==> Downloading $aar_url"
    if ! curl -fsSL --max-time 120 -o "$tmp_aar" "$aar_url"; then
        echo "!! Failed to download $aar_url" >&2
        rm -f "$tmp_aar"; rm -rf "$tmp_dir"
        return 1
    fi
    echo "==> Downloaded $(du -h "$tmp_aar" | cut -f1)"

    if ! unzip -qq -o "$tmp_aar" -d "$tmp_dir"; then
        echo "!! Failed to unzip AAR" >&2
        rm -f "$tmp_aar"; rm -rf "$tmp_dir"
        return 1
    fi

    # Layout inside AAR:
    #   prefab/modules/lsplant/include/lsplant.hpp
    #   prefab/modules/lsplant/libs/android.arm64-v8a/liblsplant.so
    #   prefab/modules/lsplant/libs/android.armeabi-v7a/liblsplant.so
    #   prefab/modules/lsplant/libs/android.x86_64/liblsplant.so
    #   prefab/modules/lsplant/libs/android.x86/liblsplant.so
    local prefab="$tmp_dir/prefab/modules/lsplant"
    if [ ! -d "$prefab" ]; then
        echo "!! AAR does not contain prefab/modules/lsplant/ — wrong artifact?" >&2
        echo "   AAR contents:"
        (cd "$tmp_dir" && find . -maxdepth 4 -type d | sed 's/^/     /')
        rm -f "$tmp_aar"; rm -rf "$tmp_dir"
        return 1
    fi

    # Copy header(s)
    if [ -d "$prefab/include" ]; then
        cp -R "$prefab/include/." "$PREBUILT/include/"
        echo "==> Copied headers to $PREBUILT/include/"
    else
        echo "!! prefab/modules/lsplant/include/ missing" >&2
        rm -f "$tmp_aar"; rm -rf "$tmp_dir"
        return 1
    fi

    # Copy per-ABI .so
    local abi_count=0
    for abi_dir in "$prefab/libs"/android.*; do
        [ -d "$abi_dir" ] || continue
        local abi="${abi_dir##*/android.}"
        local so="$abi_dir/liblsplant.so"
        if [ ! -f "$so" ]; then
            echo "!! $abi_dir has no liblsplant.so" >&2
            continue
        fi
        mkdir -p "$PREBUILT/lib/$abi"
        cp "$so" "$PREBUILT/lib/$abi/liblsplant.so"
        # Also copy module.json (per-ABI abi/api metadata)
        [ -f "$abi_dir/abi.json" ] && cp "$abi_dir/abi.json" "$PREBUILT/lib/$abi/abi.json"
        abi_count=$((abi_count + 1))
        echo "==> Copied liblsplant.so for $abi ($(du -h "$so" | cut -f1))"
    done

    rm -f "$tmp_aar"; rm -rf "$tmp_dir"

    if [ "$abi_count" = "0" ]; then
        echo "!! No ABIs found in AAR" >&2
        return 1
    fi

    # Write a version stamp so CMake / build.sh can print it
    echo "$ver" > "$PREBUILT/VERSION"
    return 0
}

# ============================================================================
# 3. Fetch Dobby (small, clean repo — no submodule pain)
# ============================================================================

fetch_dobby() {
    if [ -d "$JNI/dobby" ] && [ -f "$JNI/dobby/CMakeLists.txt" ]; then
        echo "==> dobby already present"
        return 0
    fi
    echo "==> Cloning jmpews/Dobby (shallow, no submodules)..."
    # Belt: rewrite any SSH URL to HTTPS in case Dobby ever adds SSH deps later.
    git config --global url."https://github.com/".insteadOf "git@github.com:" || true
    if ! git clone --depth 1 https://github.com/jmpews/Dobby.git "$JNI/dobby"; then
        echo "!! git clone Dobby failed" >&2
        return 1
    fi
    return 0
}

# ============================================================================
# Execute
# ============================================================================

LSPLANT_OK=1
DOBBY_OK=1
fetch_lsplant_aar "$LSPLANT_VER" || LSPLANT_OK=0
fetch_dobby                     || DOBBY_OK=0

if [ "$LSPLANT_OK" = "0" ] || [ "$DOBBY_OK" = "0" ]; then
    echo ""
    echo "!! Path B dependencies incomplete:"
    echo "     lsplant AAR = $LSPLANT_OK"
    echo "     dobby       = $DOBBY_OK"
    echo "   Build will proceed, but Path B (Java method hooks) will be disabled."
else
    echo "==> Path B dependencies fetched successfully."
    echo "    lsplant  version: $(cat "$PREBUILT/VERSION")"
    echo "    lsplant  ABIs:    $(ls "$PREBUILT/lib" | tr '\n' ' ')"
    echo "    dobby    at:      $JNI/dobby"
fi

cd "$ROOT"

# ============================================================================
# 4. Compile Java helper -> classes.dex -> C header
# ============================================================================

HAS_JAVAC=0; HAS_D8=0
command -v javac >/dev/null 2>&1 && HAS_JAVAC=1
command -v d8    >/dev/null 2>&1 && HAS_D8=1

# Try to locate d8 in Android SDK if not on PATH (common on GitHub CI).
if [ "$HAS_D8" = "0" ] && [ -n "${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}" ]; then
    SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
    D8_CAND=$(find "$SDK" -type f -name d8 2>/dev/null | head -1 || true)
    if [ -n "$D8_CAND" ]; then
        echo "==> Found d8 in SDK: $D8_CAND"
        export PATH="$(dirname "$D8_CAND"):$PATH"
        HAS_D8=1
    fi
fi

if [ -f java_helper/TernakHookHelper.java ] && [ "$HAS_JAVAC" = "1" ] && [ "$HAS_D8" = "1" ]; then
    echo "==> Compiling TernakHookHelper.java -> classes.dex -> jni/helper_dex.h"
    TMP="$(mktemp -d)"
    javac -source 8 -target 8 -d "$TMP" java_helper/TernakHookHelper.java
    d8 --output "$TMP" $(find "$TMP" -name '*.class')
    if [ ! -f "$TMP/classes.dex" ]; then
        echo "!! d8 did not produce classes.dex — Path B helper dex not embedded"
    elif ! command -v xxd >/dev/null 2>&1; then
        echo "!! xxd not found — Path B helper dex not embedded"
    else
        {
            echo "// Auto-generated by fetch_lsplant.sh — do not edit."
            xxd -i -n HELPER_DEX "$TMP/classes.dex"
            echo "static const size_t HELPER_DEX_LEN = HELPER_DEX_len;"
            echo "#define TT_HAVE_HELPER_DEX 1"
        } > "$JNI/helper_dex.h"
        echo "==> Generated jni/helper_dex.h ($(wc -c <"$TMP/classes.dex") bytes)"
    fi
    rm -rf "$TMP"
else
    echo "!! javac/d8 or java_helper/ missing — Path B compiled but hook install will fail-soft"
    echo "   (HAS_JAVAC=$HAS_JAVAC HAS_D8=$HAS_D8)"
fi

echo ""
echo "==> Done. Now run: ./build.sh"
