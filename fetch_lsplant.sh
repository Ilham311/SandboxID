#!/usr/bin/env bash
# Ternak TT — Path B fetcher (v1.2.0, AAR-only for BOTH lsplant + shadowhook)
#
# v1.2.0 pivot: ShadowHook is now consumed as an AAR from Maven Central,
# matching how we already handle lsplant. Source-based build (v1.1.7-v1.1.9)
# broke under NDK r26d Clang with hundreds of -Werror errors on
# -Wunsafe-buffer-usage, -Wdeclaration-after-statement, and
# -Wreserved-identifier — Clang policy changes newer than ShadowHook v1.0.9's
# source could anticipate. The AAR is pre-compiled by ByteDance with
# compatible flags, so we sidestep the compile entirely.
#
# Official integration path per ByteDance doc/manual.md:
#   dependencies { implementation 'com.bytedance.android:shadowhook:x.y.z' }
#   find_package(shadowhook REQUIRED CONFIG)
#   target_link_libraries(mylib shadowhook::shadowhook)
#
# We skip Gradle/prefab entirely and extract .so + header manually because
# our build is pure NDK toolchain + raw CMake (no AGP).
#
# SONAME collision protection:
#   The AAR ships libshadowhook.so with SONAME=libshadowhook.so. TikTok/Douyin
#   bundles its own libshadowhook.so (ByteDance dogfoods internally). We
#   patchelf the extracted .so's SONAME to libternak_shadowhook.so BEFORE
#   build, so libternak_tt.so bakes DT_NEEDED=libternak_shadowhook.so.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
JNI="$ROOT/jni"
PREBUILT_LS="$ROOT/prebuilt/lsplant"
PREBUILT_SH="$ROOT/prebuilt/shadowhook"

LSPLANT_FALLBACK_VERSION="6.4"
LSPLANT_MAVEN_GROUP="org/lsposed/lsplant"
LSPLANT_ARTIFACT="lsplant-standalone"

SHADOWHOOK_FALLBACK_VERSION="2.0.1"
SHADOWHOOK_MAVEN_GROUP="com/bytedance/android"
SHADOWHOOK_ARTIFACT="shadowhook"

MAVEN_BASE="https://repo1.maven.org/maven2"

echo "==> Ternak TT — Path B fetcher (v1.2.0, AAR-only)"
echo "==> Root: $ROOT"

for cmd in curl unzip; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "!! $cmd not found. Install $cmd and re-run." >&2
        exit 1
    fi
done

HAS_PATCHELF=0
if command -v patchelf >/dev/null 2>&1; then
    HAS_PATCHELF=1
    echo "==> patchelf detected: SONAME rewrite ENABLED"
else
    echo "!! patchelf not on PATH — will ship libshadowhook.so with original SONAME."
    echo "   Install with: sudo apt-get install -y patchelf"
fi

rm -rf "$PREBUILT_LS" "$PREBUILT_SH"
mkdir -p "$JNI" \
         "$PREBUILT_LS/include" "$PREBUILT_LS/lib" \
         "$PREBUILT_SH/include" "$PREBUILT_SH/lib"

resolve_maven_version() {
    local group="$1" artifact="$2" env_var="$3" fallback="$4"
    local override
    override="$(eval "printf '%s' \"\${$env_var:-}\"")"
    if [ -n "$override" ]; then
        echo "$override"; return 0
    fi
    local meta_url="${MAVEN_BASE}/${group}/${artifact}/maven-metadata.xml"
    local meta_xml latest
    meta_xml=$(curl -fsSL --max-time 30 "$meta_url" 2>/dev/null || true)
    if [ -n "$meta_xml" ]; then
        latest=$(echo "$meta_xml" | sed -n 's:.*<release>\(.*\)</release>.*:\1:p' | head -1)
        [ -z "$latest" ] && \
            latest=$(echo "$meta_xml" | sed -n 's:.*<latest>\(.*\)</latest>.*:\1:p' | head -1)
        if [ -n "$latest" ]; then
            echo "$latest"; return 0
        fi
    fi
    echo "$fallback"
}

# Prefab AAR layout (schema v2):
#   prefab/modules/<name>/include/<headers>
#   prefab/modules/<name>/libs/android.<abi>/<lib>.so
fetch_prefab_aar() {
    local group="$1" artifact="$2" ver="$3" module="$4" so_name="$5" prebuilt_dir="$6"
    local aar_url="${MAVEN_BASE}/${group}/${artifact}/${ver}/${artifact}-${ver}.aar"
    local tmp_aar tmp_dir
    tmp_aar=$(mktemp -t "${artifact}.XXXXXX.aar")
    tmp_dir=$(mktemp -d -t "${artifact}-aar.XXXXXX")

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

    local prefab_root="$tmp_dir/prefab/modules/$module"
    if [ ! -d "$prefab_root" ]; then
        echo "!! AAR does not contain prefab/modules/$module/ — wrong artifact?" >&2
        (cd "$tmp_dir" && find . -maxdepth 4 -type d | sed 's/^/     /') >&2
        rm -f "$tmp_aar"; rm -rf "$tmp_dir"
        return 1
    fi

    if [ -d "$prefab_root/include" ]; then
        cp -R "$prefab_root/include/." "$prebuilt_dir/include/"
        echo "==> $module: copied headers to $prebuilt_dir/include/"
    fi

    local abi_count=0
    for abi_dir in "$prefab_root/libs"/android.*; do
        [ -d "$abi_dir" ] || continue
        local abi="${abi_dir##*/android.}"
        case "$abi" in
            riscv64) echo "==> $module: skipping $abi"; continue ;;
        esac
        local so="$abi_dir/$so_name"
        if [ ! -f "$so" ]; then
            echo "!! $module/$abi has no $so_name" >&2
            continue
        fi
        mkdir -p "$prebuilt_dir/lib/$abi"
        cp "$so" "$prebuilt_dir/lib/$abi/$so_name"
        [ -f "$abi_dir/abi.json" ] && cp "$abi_dir/abi.json" "$prebuilt_dir/lib/$abi/abi.json"
        abi_count=$((abi_count + 1))
        echo "==> $module: copied $so_name for $abi ($(du -h "$so" | cut -f1))"
    done

    rm -f "$tmp_aar"; rm -rf "$tmp_dir"

    if [ "$abi_count" = "0" ]; then
        echo "!! $module: no ABIs extracted from AAR" >&2
        return 1
    fi
    echo "$ver" > "$prebuilt_dir/VERSION"
    return 0
}

# ============================================================================
# 1. LSPlant AAR
# ============================================================================

LSPLANT_VER=$(resolve_maven_version "$LSPLANT_MAVEN_GROUP" "$LSPLANT_ARTIFACT" \
                                    "LSPLANT_VERSION" "$LSPLANT_FALLBACK_VERSION")
echo "==> LSPlant version: $LSPLANT_VER"
LSPLANT_OK=1
fetch_prefab_aar "$LSPLANT_MAVEN_GROUP" "$LSPLANT_ARTIFACT" "$LSPLANT_VER" \
                 "lsplant" "liblsplant.so" "$PREBUILT_LS" || LSPLANT_OK=0

# ============================================================================
# 2. ShadowHook AAR (v1.2.0: replaces source-clone from v1.1.7-v1.1.9)
# ============================================================================

SHADOWHOOK_VER=$(resolve_maven_version "$SHADOWHOOK_MAVEN_GROUP" "$SHADOWHOOK_ARTIFACT" \
                                       "SHADOWHOOK_VERSION" "$SHADOWHOOK_FALLBACK_VERSION")
echo "==> ShadowHook version: $SHADOWHOOK_VER"
SHADOWHOOK_OK=1
fetch_prefab_aar "$SHADOWHOOK_MAVEN_GROUP" "$SHADOWHOOK_ARTIFACT" "$SHADOWHOOK_VER" \
                 "shadowhook" "libshadowhook.so" "$PREBUILT_SH" || SHADOWHOOK_OK=0

# ============================================================================
# 3. SONAME rewrite: libshadowhook.so -> libternak_shadowhook.so
# ============================================================================

if [ "$SHADOWHOOK_OK" = "1" ] && [ "$HAS_PATCHELF" = "1" ]; then
    for abi_dir in "$PREBUILT_SH/lib"/*; do
        [ -d "$abi_dir" ] || continue
        SO="$abi_dir/libshadowhook.so"
        [ -f "$SO" ] || continue
        if patchelf --set-soname libternak_shadowhook.so "$SO" 2>&1; then
            NEW="$abi_dir/libternak_shadowhook.so"
            mv -f "$SO" "$NEW"
            NEW_SONAME=$(patchelf --print-soname "$NEW" 2>/dev/null || echo "?")
            echo "==> ${abi_dir##*/}: SONAME rewritten -> $NEW_SONAME"
        else
            echo "!! ${abi_dir##*/}: patchelf --set-soname failed" >&2
        fi
    done
elif [ "$SHADOWHOOK_OK" = "1" ]; then
    echo "!! Warning: shipping libshadowhook.so with unchanged SONAME."
    for abi_dir in "$PREBUILT_SH/lib"/*; do
        [ -d "$abi_dir" ] || continue
        SO="$abi_dir/libshadowhook.so"
        [ -f "$SO" ] && mv -f "$SO" "$abi_dir/libternak_shadowhook.so"
    done
fi

if [ "$LSPLANT_OK" = "0" ] || [ "$SHADOWHOOK_OK" = "0" ]; then
    echo ""
    echo "!! Path B dependencies incomplete: lsplant=$LSPLANT_OK shadowhook=$SHADOWHOOK_OK"
else
    echo "==> Path B fetched OK: lsplant $(cat "$PREBUILT_LS/VERSION") + shadowhook $(cat "$PREBUILT_SH/VERSION")"
fi

cd "$ROOT"

# ============================================================================
# 4. Compile Java helper -> classes.dex -> C header (unchanged from v1.1.7)
# ============================================================================

HAS_JAVAC=0; HAS_D8=0
command -v javac >/dev/null 2>&1 && HAS_JAVAC=1
command -v d8    >/dev/null 2>&1 && HAS_D8=1

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
    TMP="$(mktemp -d)"
    if [ -n "$ANDROID_JAR" ]; then
        javac -source 8 -target 8 -bootclasspath "$ANDROID_JAR" -d "$TMP" java_helper/TernakHookHelper.java
    else
        javac -source 8 -target 8 -d "$TMP" java_helper/TernakHookHelper.java
    fi
    d8 --output "$TMP" $(find "$TMP" -name '*.class' 2>/dev/null) 2>/dev/null || true
    if [ ! -f "$TMP/classes.dex" ]; then
        echo "!! d8 did not produce classes.dex"
    elif ! command -v xxd >/dev/null 2>&1; then
        echo "!! xxd not found"
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
