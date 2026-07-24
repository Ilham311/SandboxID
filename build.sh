#!/usr/bin/env bash
# ============================================================
# Ternak TT v1.0 - Build script (all ABIs -> flashable zip)
# ============================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to your NDK path (r26+)}"
MIN_SDK="${MIN_SDK:-33}"
VERSION="$(grep '^version=' module.prop | cut -d= -f2)"
OUT="$ROOT/dist"
PKG="$ROOT/pkg"

echo "==> Ternak TT $VERSION"
echo "==> NDK: $ANDROID_NDK_HOME"

# 1. Fetch zygisk.hpp if missing
if [ ! -f jni/zygisk.hpp ]; then
  echo "==> Fetching zygisk.hpp"
  curl -fsSL -o jni/zygisk.hpp \
    https://raw.githubusercontent.com/topjohnwu/zygisk-module-sample/master/module/jni/zygisk.hpp
fi

# 2. Build per ABI
ABIS=(arm64-v8a armeabi-v7a x86_64 x86)
for ABI in "${ABIS[@]}"; do
  echo "==> Building $ABI"
  BUILD="build/$ABI"
  rm -rf "$BUILD"
  mkdir -p "$BUILD"
  cmake -S jni -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="android-$MIN_SDK" \
    -DCMAKE_BUILD_TYPE=Release
  cmake --build "$BUILD" -j
done

# 3. Assemble package
rm -rf "$PKG" "$OUT"
mkdir -p "$PKG/zygisk" "$PKG/bin" "$OUT"

# module scripts
cp module.prop action.sh service.sh customize.sh "$PKG/"

# .so per ABI (Zygisk expects zygisk/<abi>.so)
cp build/arm64-v8a/libternak_tt.so    "$PKG/zygisk/arm64-v8a.so"
cp build/armeabi-v7a/libternak_tt.so  "$PKG/zygisk/armeabi-v7a.so"
cp build/x86_64/libternak_tt.so       "$PKG/zygisk/x86_64.so"
cp build/x86/libternak_tt.so          "$PKG/zygisk/x86.so"

# CLI per ABI
cp build/arm64-v8a/ternak-tt    "$PKG/bin/ternak-tt-arm64"
cp build/armeabi-v7a/ternak-tt  "$PKG/bin/ternak-tt-arm"
cp build/x86_64/ternak-tt       "$PKG/bin/ternak-tt-x86_64"
cp build/x86/ternak-tt          "$PKG/bin/ternak-tt-x86"

# resetprop-rs (must be provided)
if [ -f prebuilt/resetprop-rs ]; then
  cp prebuilt/resetprop-rs "$PKG/bin/resetprop-rs"
else
  echo "WARN: prebuilt/resetprop-rs missing; native prop apply will be skipped at runtime"
fi

# 4. Zip (flashable format: files at zip root)
cd "$PKG"
zip -r9 "$OUT/ternak-tt-$VERSION.zip" . -x "*.DS_Store"
cd "$ROOT"

echo ""
echo "==> Built: $OUT/ternak-tt-$VERSION.zip"
ls -lh "$OUT/ternak-tt-$VERSION.zip"
