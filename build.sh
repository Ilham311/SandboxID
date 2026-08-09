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
  [ -f summarize.sh ] && cp summarize.sh "$PKG/"
  [ -f post-fs-data.sh ] && cp post-fs-data.sh "$PKG/"
  [ -f target.txt ] && cp target.txt "$PKG/"
  [ -f helpers.sh ] && cp helpers.sh "$PKG/"
  [ -f rotate_ids.sh ] && cp rotate_ids.sh "$PKG/"
  [ -f create_profile.sh ] && cp create_profile.sh "$PKG/"
  [ -f add_target.sh ] && cp add_target.sh "$PKG/"
  [ -f remove_profile.sh ] && cp remove_profile.sh "$PKG/"
  [ -f check_profile.sh ] && cp check_profile.sh "$PKG/"
  [ -f uninstall.sh ] && cp uninstall.sh "$PKG/"
  [ -d webroot ] && cp -R webroot "$PKG/"

  if [ -d dpc ]; then
    echo "  ==> Building DPC APK"
    ./gradlew :dpc:assembleRelease
    mkdir -p "$PKG/system/priv-app/TernakTTDpc"
    # Sign it using the self-signed keystore if needed, but since gradle will
    # generate an unsigned apk, we will just use that for now and user signs it or we create keystore
    if [ ! -f dpc/ternak-dpc.jks ]; then
        keytool -genkey -v -keystore dpc/ternak-dpc.jks -keyalg RSA -keysize 2048 -validity 10000 -alias dpc -storepass password -keypass password -dname "CN=TernakTT, OU=DPC, O=Ternak, L=Jakarta, ST=DKI, C=ID"
    fi
    /opt/android-sdk/build-tools/33.0.1/apksigner sign --ks dpc/ternak-dpc.jks --ks-pass pass:password dpc/build/outputs/apk/release/dpc-release-unsigned.apk
    cp dpc/build/outputs/apk/release/dpc-release-unsigned.apk "$PKG/system/priv-app/TernakTTDpc/TernakTTDpc.apk"
  fi

  if [ "$V" = "debug" ]; then
    sed -i 's/^name=.*/&  [DEBUG]/' "$PKG/module.prop"
    echo "variant=debug"              >  "$PKG/debug_variant"
    echo "created=$(date -u +%FT%TZ)" >> "$PKG/debug_variant"
    echo "version=$VERSION"           >> "$PKG/debug_variant"
    mkdir -p "$PKG/debug"
    echo "Auto-populated by service.sh on boot. Latest session log lives here." \
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

  # Clean up staging directory
  rm -rf "$PKG"
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
