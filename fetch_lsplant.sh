#!/usr/bin/env bash
# Ternak TT — Path B fetcher (v1.1.7, AAR + ShadowHook)
#
# Strategy:
#   1. Download lsplant-standalone AAR from Maven Central. Extract prefab .so
#      per ABI + lsplant.hpp header into prebuilt/lsplant/.
#      "standalone" = statically-linked libc++ (no libc++_shared.so dep).
#      This eliminates the v1.1.5-era pain of cloning LSPlant from source with
#      its SSH-URL test/ submodules.
#   2. git clone bytedance/android-inline-hook (ShadowHook, pinned tag) into
#      jni/shadowhook/. LSPlant needs user-supplied inline_hooker; ShadowHook
#      is actively maintained by ByteDance (battle-tested against exactly the
#      anti-tamper surfaces we're spoofing) and replaces jmpews/Dobby, whose
#      master branch stopped compiling on NDK r26d in v1.1.6 CI (ADRP ASM,
#      RuntimeModule::load_address rename, missing core/arch/Cpu.h, ...).
#   3. javac + d8 compile java_helper/TernakHookHelper.java to jni/helper_dex.h.
#      v1.1.7 Bug #1 fix: locate android.jar via ANDROID_SDK_ROOT/platforms/
#      android-<API>/ so javac -bootclasspath can resolve android.content.*.
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

echo "==> Ternak TT — Path B lsplant fetcher (v1.1.7, AAR + ShadowHook)"
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
        # v1.1.7: skip riscv64 — NDK r26d builds don't target it and we
        # don't ship it (waste elimination, ~88K per build).
        case "$abi" in
            riscv64) echo "==> Skipping $abi (not built by this module)"; continue ;;
        esac
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
# 3. Fetch ShadowHook (bytedance/android-inline-hook, pinned tag)
#
#    ShadowHook replaces jmpews/Dobby as of v1.1.7. Dobby's master branch
#    stopped compiling on NDK r26d — v1.1.6 CI showed:
#      * closure_bridge_arm64.asm: invalid symbol kind for ADRP relocation
#      * os_arch_features.h: undeclared 'OSMemory' / 'kReadExecute'
#      * ProcessRuntime.cc + dobby_symbol_resolver.cc: no member 'load_address'
#      * code-patch-tool-posix.cc: 'core/arch/Cpu.h' file not found
#    ShadowHook is stable, actively maintained, has a clean CMake integration,
#    and its API (shadowhook_hook_func_addr / shadowhook_unhook) maps cleanly
#    onto lsplant::InitInfo::{inline_hooker, inline_unhooker}.
# ============================================================================

SHADOWHOOK_TAG="${SHADOWHOOK_TAG:-v1.0.9}"

fetch_shadowhook() {
    local marker="$JNI/shadowhook/shadowhook/src/main/cpp/CMakeLists.txt"
    if [ -f "$marker" ]; then
        echo "==> shadowhook already present ($JNI/shadowhook)"
        return 0
    fi
    echo "==> Cloning bytedance/android-inline-hook@${SHADOWHOOK_TAG} (shallow)..."
    # Belt: rewrite any SSH URL to HTTPS in case ShadowHook ever adds SSH deps.
    git config --global url."https://github.com/".insteadOf "git@github.com:" || true
    rm -rf "$JNI/shadowhook"
    if ! git clone --depth 1 --branch "$SHADOWHOOK_TAG" \
            https://github.com/bytedance/android-inline-hook.git "$JNI/shadowhook"; then
        echo "!! git clone ShadowHook @${SHADOWHOOK_TAG} failed" >&2
        return 1
    fi
    # Sanity-check the CMake entry point we depend on. If the repo layout
    # ever changes, print a diagnostic so the next iteration can adjust.
    if [ ! -f "$marker" ]; then
        echo "!! ShadowHook CMakeLists.txt not at expected path — layout changed?" >&2
        echo "   Expected: $marker" >&2
        echo "   Actual CMakeLists.txt files under $JNI/shadowhook:" >&2
        find "$JNI/shadowhook" -maxdepth 6 -name CMakeLists.txt 2>/dev/null | sed 's/^/     /' >&2
        return 1
    fi
    return 0
}

# ============================================================================
# Execute
# ============================================================================

LSPLANT_OK=1
SHADOWHOOK_OK=1
fetch_lsplant_aar "$LSPLANT_VER" || LSPLANT_OK=0
fetch_shadowhook                 || SHADOWHOOK_OK=0

if [ "$LSPLANT_OK" = "0" ] || [ "$SHADOWHOOK_OK" = "0" ]; then
    echo ""
    echo "!! Path B dependencies incomplete:"
    echo "     lsplant AAR = $LSPLANT_OK"
    echo "     shadowhook  = $SHADOWHOOK_OK"
    echo "   Build will proceed, but Path B (Java method hooks) will be disabled."
else
    echo "==> Path B dependencies fetched successfully."
    echo "    lsplant     version: $(cat "$PREBUILT/VERSION")"
    echo "    lsplant     ABIs:    $(ls "$PREBUILT/lib" | tr '\n' ' ')"
    echo "    shadowhook  tag:     ${SHADOWHOOK_TAG}"
    echo "    shadowhook  at:      $JNI/shadowhook"
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

    # v1.1.7 Bug #1 fix: locate android.jar for javac -bootclasspath.
    # Without it, v1.1.6 CI hit `error: package android.content does not exist`
    # for ContentResolver, javac produced 0 .class files, and d8 never made
    # classes.dex — so Path B lost its 5 Java-side hooks (Settings.Secure.*,
    # Settings.Global.*) even though the native side compiled fine.
    # Requires the CI workflow to install `platforms;android-34` (see build.yml).
    ANDROID_JAR=""
    SDK_ROOT="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
    if [ -n "$SDK_ROOT" ]; then
        for API in 34 35 33 36 32 31; do
            CAND="$SDK_ROOT/platforms/android-$API/android.jar"
            if [ -f "$CAND" ]; then
                ANDROID_JAR="$CAND"
                echo "==> Using android.jar from android-$API"
                break
            fi
        done
    fi
    if [ -z "$ANDROID_JAR" ]; then
        echo "!! android.jar not found under \$ANDROID_SDK_ROOT/platforms/android-{34,35,33,36,32,31}/"
        echo "   Ensure the workflow installs 'platforms;android-34' via android-actions/setup-android."
        echo "   Falling back to javac without -bootclasspath — expect ContentResolver resolution failure."
    fi

    TMP="$(mktemp -d)"
    if [ -n "$ANDROID_JAR" ]; then
        javac -source 8 -target 8 -bootclasspath "$ANDROID_JAR" \
              -d "$TMP" java_helper/TernakHookHelper.java
    else
        javac -source 8 -target 8 -d "$TMP" java_helper/TernakHookHelper.java
    fi
    d8 --output "$TMP" $(find "$TMP" -name '*.class' 2>/dev/null) 2>/dev/null || true
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
