#!/usr/bin/env bash
# build.sh - build Ternak TT release and debug variants.
#
# v1.1.0 refactor: build_variant() was 67 lines and did five different things.
# It is now split into narrow helpers:
#   stage_module_files    - copy module/script files into $VARIANT_DIR
#   apply_debug_marks     - stamp debug variant metadata + touch debug marker
#   copy_native_binaries  - copy .so and CLI per-ABI
#   pack_variant_zip      - zip up $VARIANT_DIR into $OUT_ZIP

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

DIST="$ROOT/dist"
STAGE="$ROOT/.stage"
rm -rf "$DIST" "$STAGE"
mkdir -p "$DIST" "$STAGE"

VERSION="$(awk -F= '$1=="version" {print $2}' "$ROOT/module.prop")"
VERSION_CODE="$(awk -F= '$1=="versionCode" {print $2}' "$ROOT/module.prop")"
[[ -z "$VERSION" || -z "$VERSION_CODE" ]] && { echo "module.prop missing version/versionCode" >&2; exit 1; }
echo "[build] Ternak TT $VERSION (code $VERSION_CODE)"

# ----- helpers ---------------------------------------------------------------

stage_module_files() {
    local dst="$1"
    mkdir -p "$dst"
    for f in module.prop customize.sh service.sh post-fs-data.sh action.sh \
             helpers.sh rotate_ids.sh target.txt update.json; do
        [[ -f "$ROOT/$f" ]] && cp -f "$ROOT/$f" "$dst/$f"
    done
    if [[ -d "$ROOT/webroot" ]]; then
        mkdir -p "$dst/webroot"
        cp -rf "$ROOT/webroot/." "$dst/webroot/"
    fi
}

apply_debug_marks() {
    local dst="$1" variant="$2"
    if [[ "$variant" == "debug" ]]; then
        touch "$dst/debug_variant"
        sed -i.bak \
            -e "s/^name=.*/name=Ternak TT $VERSION (DEBUG)/" \
            -e "s/^version=.*/version=$VERSION-debug/" \
            "$dst/module.prop"
        rm -f "$dst/module.prop.bak"
    fi
}

copy_native_binaries() {
    local dst="$1" variant="$2"
    mkdir -p "$dst/bin" "$dst/zygisk"
    local suffix=""
    [[ "$variant" == "debug" ]] && suffix="-debug"
    for abi in arm64-v8a armeabi-v7a x86_64 x86; do
        local so="$ROOT/prebuilt/$abi/libternak_tt${suffix}.so"
        local cli="$ROOT/prebuilt/$abi/ternak-tt${suffix}"
        local zabi="$abi"
        local cliname=""
        case "$abi" in
            arm64-v8a)   cliname="ternak-tt-arm64" ;;
            armeabi-v7a) cliname="ternak-tt-arm"   ;;
            x86_64)      cliname="ternak-tt-x86_64";;
            x86)         cliname="ternak-tt-x86"   ;;
        esac
        if [[ -f "$so" ]]; then
            cp -f "$so" "$dst/zygisk/${zabi}.so"
        else
            echo "[build] WARN: missing $so" >&2
        fi
        if [[ -f "$cli" ]]; then
            cp -f "$cli" "$dst/bin/$cliname"
            chmod 0755 "$dst/bin/$cliname"
        else
            echo "[build] WARN: missing $cli" >&2
        fi
    done
}

pack_variant_zip() {
    local dst="$1" outzip="$2"
    (cd "$dst" && zip -qr9 "$outzip" .)
    echo "[build] wrote $outzip ($(du -h "$outzip" | cut -f1))"
}

# ----- pipeline --------------------------------------------------------------

build_variant() {
    local variant="$1"
    local vdir="$STAGE/$variant"
    local outzip="$DIST/ternak-tt-$VERSION-${variant}.zip"
    echo "[build] === variant: $variant ==="
    rm -rf "$vdir"
    stage_module_files      "$vdir"
    apply_debug_marks       "$vdir" "$variant"
    copy_native_binaries    "$vdir" "$variant"
    pack_variant_zip        "$vdir" "$outzip"
}

for v in release debug; do
    build_variant "$v"
done

echo "[build] done. artifacts under $DIST/"
ls -1 "$DIST"
