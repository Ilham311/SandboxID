#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to your NDK path (r26+)}"
MIN_SDK="${MIN_SDK:-33}"
VARIANT="${VARIANT:-both}"
VERSION="$(grep '^version=' module.prop | cut -d= -f2)"
OUT="$ROOT/dist"

ABIS=(arm64-v8a armeabi-v7a x86_64 x86)

echo "==> Ternak TT $VERSION"
echo "==> NDK: $ANDROID_NDK_HOME"
echo "==> Variant(s): $VARIANT"

if [ ! -f jni/zygisk.hpp ]; then
  echo "==> Fetching zygisk.hpp"
  curl -fsSL -o jni/zygisk.hpp \
    https://raw.githubusercontent.com/topjohnwu/zygisk-module-sample/master/module/jni/zygisk.hpp
fi

# v1.2.0 Path B: both lsplant AND shadowhook are now prebuilt AARs from
# Maven Central. Source-based shadowhook (v1.1.7-v1.1.9) is retired — it
# broke under NDK r26d Clang with -Werror cascade on -Wunsafe-buffer-usage,
# -Wdeclaration-after-statement, -Wreserved-identifier. AAR is pre-compiled
# by ByteDance and sidesteps the compile entirely.
PATH_B_OK=1
if [ ! -d prebuilt/lsplant/lib ] || [ ! -f prebuilt/lsplant/include/lsplant.hpp ]; then
  echo "==> Path B disabled: prebuilt/lsplant/ incomplete (need AAR-extracted .so + header)."
  PATH_B_OK=0
fi
if [ ! -d prebuilt/shadowhook/lib ] || [ ! -f prebuilt/shadowhook/include/shadowhook.h ]; then
  echo "==> Path B disabled: prebuilt/shadowhook/ incomplete (need AAR-extracted .so + header)."
  PATH_B_OK=0
fi
if [ "$PATH_B_OK" = "1" ]; then
  LSPLANT_VER="$(cat prebuilt/lsplant/VERSION 2>/dev/null || echo unknown)"
  SHADOWHOOK_VER="$(cat prebuilt/shadowhook/VERSION 2>/dev/null || echo unknown)"
  echo "==> Path B: lsplant $LSPLANT_VER + shadowhook $SHADOWHOOK_VER (AAR) -> TT_HAVE_LSPLANT will be enabled"
else
  echo "    Run './fetch_lsplant.sh' first if you want Java method hooks (Settings.Secure, MediaDrm, ...)."
fi

mkdir -p "$OUT"

build_variant() {
  local V="$1"
  local DBG_FLAG
  case "$V" in
    debug)   DBG_FLAG="-DTERNAK_TT_DEBUG=ON"  ;;
    release) DBG_FLAG="-DTERNAK_TT_DEBUG=OFF" ;;
    *) echo "unknown variant: $V" >&2; return 1 ;;
  esac

  local PKG="$ROOT/pkg-$V"
  echo ""
  echo "============================================================"
  echo "  Building variant: $V"
  echo "============================================================"

  for ABI in "${ABIS[@]}"; do
    echo "  ==> [$V] $ABI"
    local BUILD="build/$V/$ABI"
    rm -rf "$BUILD"
    mkdir -p "$BUILD"
    cmake -S jni -B "$BUILD" \
      -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI="$ABI" \
      -DANDROID_PLATFORM="android-$MIN_SDK" \
      -DCMAKE_BUILD_TYPE=Release \
      $DBG_FLAG >/dev/null
    cmake --build "$BUILD" -j
  done

  rm -rf "$PKG"
  mkdir -p "$PKG/zygisk" "$PKG/bin"
  cp module.prop action.sh service.sh customize.sh "$PKG/"

  # v1.2.0: ship liblsplant.so + libternak_shadowhook.so via Magisk/KSU
  # $MODPATH/system/lib{,64} overlay so DT_NEEDED resolves inside app
  # processes at runtime. Both .so files come directly from prebuilt AARs
  # (see fetch_lsplant.sh). libshadowhook.so's SONAME was rewritten to
  # libternak_shadowhook.so by patchelf during fetch, to avoid collision
  # with target apps that bundle their own libshadowhook.so (e.g. TikTok).
  if [ "${PATH_B_OK:-0}" = "1" ]; then
    mkdir -p "$PKG/system/lib64" "$PKG/system/lib"
    # arm64-v8a + x86_64 -> /system/lib64
    for A64 in arm64-v8a x86_64; do
      if [ -f "prebuilt/lsplant/lib/$A64/liblsplant.so" ]; then
        cp "prebuilt/lsplant/lib/$A64/liblsplant.so" "$PKG/system/lib64/liblsplant.so.$A64"
      fi
      if [ -f "prebuilt/shadowhook/lib/$A64/libternak_shadowhook.so" ]; then
        cp "prebuilt/shadowhook/lib/$A64/libternak_shadowhook.so" \
           "$PKG/system/lib64/libternak_shadowhook.so.$A64"
      else
        echo "  WARN: [$V] $A64: libternak_shadowhook.so missing in prebuilt/shadowhook/"
      fi
    done
    # armeabi-v7a + x86 -> /system/lib
    for A32 in armeabi-v7a x86; do
      if [ -f "prebuilt/lsplant/lib/$A32/liblsplant.so" ]; then
        cp "prebuilt/lsplant/lib/$A32/liblsplant.so" "$PKG/system/lib/liblsplant.so.$A32"
      fi
      if [ -f "prebuilt/shadowhook/lib/$A32/libternak_shadowhook.so" ]; then
        cp "prebuilt/shadowhook/lib/$A32/libternak_shadowhook.so" \
           "$PKG/system/lib/libternak_shadowhook.so.$A32"
      else
        echo "  WARN: [$V] $A32: libternak_shadowhook.so missing in prebuilt/shadowhook/"
      fi
    done
    # customize.sh will rename the correct .so.$ABI to .so at install time based
    # on device arch (single-arch install semantics for Magisk/KSU modules).
    echo "path_b_libs=shipped" > "$PKG/.path_b_stamp"
  fi
  [ -f summarize.sh ] && cp summarize.sh "$PKG/"
  [ -f post-fs-data.sh ] && cp post-fs-data.sh "$PKG/"
  [ -f target.txt ] && cp target.txt "$PKG/"

  if [ "$V" = "debug" ]; then
    sed -i 's/^name=.*/&  [DEBUG]/' "$PKG/module.prop"
    echo "variant=debug" >  "$PKG/debug_variant"
    echo "created=$(date -u +%FT%TZ)" >> "$PKG/debug_variant"
    echo "version=$VERSION" >> "$PKG/debug_variant"
    mkdir -p "$PKG/debug"
    echo "# Auto-populated by service.sh on boot. Latest session-YYYYMMDD-HHMMSS.log lives here." \
      > "$PKG/debug/README.txt"
  fi

  for ABI in "${ABIS[@]}"; do
    cp "build/$V/$ABI/libternak_tt.so" "$PKG/zygisk/$ABI.so"
  done

  cp "build/$V/arm64-v8a/ternak-tt"    "$PKG/bin/ternak-tt-arm64"
  cp "build/$V/armeabi-v7a/ternak-tt"  "$PKG/bin/ternak-tt-arm"
  cp "build/$V/x86_64/ternak-tt"       "$PKG/bin/ternak-tt-x86_64"
  cp "build/$V/x86/ternak-tt"          "$PKG/bin/ternak-tt-x86"

  if [ -f prebuilt/resetprop-rs ]; then
    cp prebuilt/resetprop-rs "$PKG/bin/resetprop-rs"
  else
    echo "  WARN: prebuilt/resetprop-rs missing; native prop apply will be skipped at runtime"
  fi

  local ZIP="$OUT/ternak-tt-$VERSION-$V.zip"
  (cd "$PKG" && zip -r9 "$ZIP" . -x "*.DS_Store" >/dev/null)
  echo "  ==> Built: $ZIP ($(du -h "$ZIP" | cut -f1))"
}

case "$VARIANT" in
  release) build_variant release ;;
  debug)   build_variant debug   ;;
  both)    build_variant release; build_variant debug ;;
  *) echo "Invalid VARIANT: $VARIANT (expected: release|debug|both)" >&2; exit 1 ;;
esac

echo ""
echo "==> All artifacts:"
ls -lh "$OUT"/*.zip
