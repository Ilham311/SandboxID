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

# v1.1.8 Path B (AAR-based): warn if lsplant .so or ShadowHook missing.
# v1.1.8 Bug #4 fix: check jni/shadowhook/ not jni/dobby/ (Dobby was replaced
# by bytedance/android-inline-hook in v1.1.7 after Dobby broke on NDK r26d).
PATH_B_OK=1
if [ ! -d prebuilt/lsplant/lib ] || [ ! -f prebuilt/lsplant/include/lsplant.hpp ]; then
  echo "==> Path B disabled: prebuilt/lsplant/ incomplete (need AAR-extracted .so + header)."
  PATH_B_OK=0
fi
if [ ! -f jni/shadowhook/shadowhook/src/main/cpp/CMakeLists.txt ]; then
  echo "==> Path B disabled: jni/shadowhook/ missing (bytedance/android-inline-hook not cloned)."
  PATH_B_OK=0
fi
if [ "$PATH_B_OK" = "1" ]; then
  LSPLANT_VER="$(cat prebuilt/lsplant/VERSION 2>/dev/null || echo unknown)"
  echo "==> Path B: prebuilt lsplant $LSPLANT_VER + jni/shadowhook present -> TT_HAVE_LSPLANT will be enabled"
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
    # v1.1.9: dropped -DCMAKE_POLICY_VERSION_MINIMUM=3.5 from v1.1.8 because
    # CMake 3.31.6 flagged it as "unused" (it takes effect only when the
    # project itself calls cmake_minimum_required below its floor, which we
    # no longer do since bumping to 3.22.1). The NDK-toolchain deprecation
    # warnings are noisy but harmless.
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

  # v1.1.7: ship liblsplant.so + libternak_shadowhook.so via Magisk/KSU
  # $MODPATH/system/lib{,64} overlay so DT_NEEDED resolves inside app
  # processes at runtime. (v1.1.6 shipped libdobby.so; ShadowHook replaced
  # it after Dobby broke on NDK r26d. The .so is renamed to
  # libternak_shadowhook.so via SONAME to avoid collision with target apps
  # that already bundle their own libshadowhook.so — e.g. TikTok itself.)
  if [ "${PATH_B_OK:-0}" = "1" ]; then
    mkdir -p "$PKG/system/lib64" "$PKG/system/lib"
    copy_shadowhook_so() {
      local abi="$1"; local dst_dir="$2"
      local src
      src=$(find "build/$V/$abi/shadowhook-build" -name 'libternak_shadowhook.so' 2>/dev/null | head -1)
      # Fallback: some CMake versions emit under a nested subdir.
      if [ -z "$src" ]; then
        src=$(find "build/$V/$abi" -name 'libternak_shadowhook.so' 2>/dev/null | head -1)
      fi
      if [ -n "$src" ] && [ -f "$src" ]; then
        cp "$src" "$dst_dir/libternak_shadowhook.so.$abi"
      else
        echo "  WARN: [$V] $abi: libternak_shadowhook.so not found under build/$V/$abi/"
      fi
    }
    # arm64-v8a + x86_64 -> /system/lib64
    for A64 in arm64-v8a x86_64; do
      if [ -f "prebuilt/lsplant/lib/$A64/liblsplant.so" ]; then
        cp "prebuilt/lsplant/lib/$A64/liblsplant.so" "$PKG/system/lib64/liblsplant.so.$A64"
      fi
      copy_shadowhook_so "$A64" "$PKG/system/lib64"
    done
    # armeabi-v7a + x86 -> /system/lib
    for A32 in armeabi-v7a x86; do
      if [ -f "prebuilt/lsplant/lib/$A32/liblsplant.so" ]; then
        cp "prebuilt/lsplant/lib/$A32/liblsplant.so" "$PKG/system/lib/liblsplant.so.$A32"
      fi
      copy_shadowhook_so "$A32" "$PKG/system/lib"
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
