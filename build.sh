#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to your NDK path (r26+)}"
MIN_SDK="${MIN_SDK:-33}"
VARIANT="${VARIANT:-both}"
VERSION="$(grep '^version=' module.prop | cut -d= -f2)"

LSP_CMAKE=""
if [ "${SBX_ENABLE_LSPLANT:-OFF}" = "ON" ]; then LSP_CMAKE="-DSBX_ENABLE_LSPLANT=ON"; fi
OUT="$ROOT/dist"

ABIS=(arm64-v8a armeabi-v7a x86_64 x86)

echo "==> SandboxID $VERSION"
echo "==> NDK: $ANDROID_NDK_HOME"
echo "==> Variant(s): $VARIANT"



ZYGISK_HPP_COMMIT="8ce26128f81baaed0b969aaf7f52f886b61af4ab"
ZYGISK_HPP_SHA256="f8d55e8b4f89d418c5941afe62ce6a09ddec1f4afd9a1b0a01eb40a93310dd28"
if [ ! -f jni/zygisk.hpp ]; then
  echo "==> Fetching zygisk.hpp @ ${ZYGISK_HPP_COMMIT}"
  curl -fsSL -o jni/zygisk.hpp \
    "https://raw.githubusercontent.com/topjohnwu/zygisk-module-sample/${ZYGISK_HPP_COMMIT}/module/jni/zygisk.hpp"
fi
if command -v sha256sum >/dev/null 2>&1; then
  GOT_HPP="$(sha256sum jni/zygisk.hpp | cut -d' ' -f1)"
elif command -v shasum >/dev/null 2>&1; then
  GOT_HPP="$(shasum -a 256 jni/zygisk.hpp | cut -d' ' -f1)"
else
  echo "ERROR: no sha256 tool (sha256sum/shasum) to verify zygisk.hpp" >&2; exit 1
fi
if [ "$GOT_HPP" != "$ZYGISK_HPP_SHA256" ]; then
  echo "ERROR: zygisk.hpp checksum mismatch — refusing to build" >&2
  echo "  expected $ZYGISK_HPP_SHA256" >&2
  echo "  got      $GOT_HPP" >&2
  echo "  delete jni/zygisk.hpp to re-fetch from pinned commit ${ZYGISK_HPP_COMMIT}" >&2
  exit 1
fi
echo "==> zygisk.hpp verified"

mkdir -p "$OUT"

build_variant() {
  local V="$1"
  local DBG_FLAG
  case "$V" in
    debug)   DBG_FLAG="-DSBX_DEBUG=ON"  ;;
    release) DBG_FLAG="-DSBX_DEBUG=OFF" ;;
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
      $DBG_FLAG ${LSP_CMAKE:-} >/dev/null
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
  [ -f personas.tsv ] && cp personas.tsv "$PKG/"
  [ -f autopif.sh ] && cp autopif.sh "$PKG/"
  [ -d webroot ] && cp -R webroot "$PKG/"

  # Opt-in: refresh the PACKAGED persona pool from Google's live Pixel build
  # data at build time (CI has curl, unlike most devices). Mutates only the
  # $PKG copy, never the source tree. Enable with AUTOPIF_REFRESH=1.
  if [ "${AUTOPIF_REFRESH:-0}" = "1" ] && [ -f "$PKG/autopif.sh" ]; then
    echo "  ==> refreshing persona pool (autopif, build-time)"
    PERSONAS_FILE="$PKG/personas.tsv" MODDIR="$PKG" sh "$PKG/autopif.sh" || true
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
    cp "build/$V/$ABI/libsandboxid.so" "$PKG/zygisk/$ABI.so"
  done

  cp "build/$V/arm64-v8a/sandboxid"    "$PKG/bin/sandboxid-arm64"
  cp "build/$V/armeabi-v7a/sandboxid"  "$PKG/bin/sandboxid-arm"
  cp "build/$V/x86_64/sandboxid"       "$PKG/bin/sandboxid-x86_64"
  cp "build/$V/x86/sandboxid"          "$PKG/bin/sandboxid-x86"

  if [ -f prebuilt/resetprop-rs ]; then
    
    if [ -f prebuilt/resetprop-rs.sha256 ] && command -v sha256sum >/dev/null 2>&1; then
      ( cd prebuilt && sha256sum -c resetprop-rs.sha256 >/dev/null ) || {
        echo "  ERROR: prebuilt/resetprop-rs checksum mismatch — refusing to package" >&2
        exit 1
      }
      echo "  ==> resetprop-rs verified"
    else
      echo "  WARN: cannot verify resetprop-rs checksum (missing .sha256 or sha256sum)" >&2
    fi
    cp prebuilt/resetprop-rs "$PKG/bin/resetprop-rs"
    
    [ -f prebuilt/resetprop-rs.sha256 ] && cp prebuilt/resetprop-rs.sha256 "$PKG/bin/resetprop-rs.sha256"
  else
    echo "  WARN: prebuilt/resetprop-rs missing; native prop apply will rely on Magisk resetprop"
  fi

  local ZIP="$OUT/sandboxid-$VERSION-$V.zip"
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
