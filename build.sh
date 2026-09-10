#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to your NDK path (r26+)}"
MIN_SDK="${MIN_SDK:-26}"
VARIANT="${VARIANT:-both}"

for _tool in cmake zip; do
  if ! command -v "$_tool" >/dev/null 2>&1; then
    echo "ERROR: required tool '$_tool' not found in PATH" >&2
    exit 1
  fi
done
if [ ! -f jni/zygisk.hpp ] && ! command -v curl >/dev/null 2>&1; then
  echo "ERROR: curl not found in PATH and jni/zygisk.hpp is not cached" >&2
  echo "  (curl is required to fetch the pinned Zygisk API header)" >&2
  exit 1
fi

if [ ! -f module.prop ]; then
  echo "ERROR: module.prop not found in $ROOT — refusing to build" >&2
  exit 1
fi
MANDATORY_RUNTIME=(
  action.sh service.sh customize.sh post-fs-data.sh helpers.sh rotate_ids.sh
  selftest.sh autopif.sh target.txt webroot/index.html webroot/app.js
  webroot/style.css webroot/theme-init.js tests/package_manifest_test.sh
)
for _required in "${MANDATORY_RUNTIME[@]}"; do
  if [ ! -f "$_required" ]; then
    echo "ERROR: mandatory runtime input missing or not a regular file: $_required" >&2
    exit 1
  fi
done
VERSION="$(grep '^version=' module.prop | cut -d= -f2 || true)"
if [ -z "${VERSION:-}" ]; then
  echo "ERROR: version= line missing in module.prop" >&2
  exit 1
fi

OUT="$ROOT/dist"

ABIS=(arm64-v8a armeabi-v7a x86_64 x86)

echo "==> SandboxID $VERSION"
echo "==> NDK:        $ANDROID_NDK_HOME"
echo "==> MIN_SDK:    $MIN_SDK"
echo "==> Variant(s): $VARIANT"

ZYGISK_HPP_COMMIT="8ce26128f81baaed0b969aaf7f52f886b61af4ab"
ZYGISK_HPP_SHA256="f8d55e8b4f89d418c5941afe62ce6a09ddec1f4afd9a1b0a01eb40a93310dd28"
if [ ! -f jni/zygisk.hpp ]; then
  echo "==> Fetching zygisk.hpp @ ${ZYGISK_HPP_COMMIT}"
  if ! curl -fsSL -o jni/zygisk.hpp \
      "https://raw.githubusercontent.com/topjohnwu/zygisk-module-sample/${ZYGISK_HPP_COMMIT}/module/jni/zygisk.hpp"; then
    echo "ERROR: failed to fetch zygisk.hpp from pinned commit ${ZYGISK_HPP_COMMIT}" >&2
    echo "  check network access to raw.githubusercontent.com" >&2
    exit 1
  fi
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
    # shellcheck disable=SC2086
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
  cp module.prop action.sh service.sh customize.sh post-fs-data.sh helpers.sh \
    rotate_ids.sh selftest.sh autopif.sh target.txt "$PKG/"
  cp -R webroot "$PKG/"
  [ -f LICENSE ]   && cp LICENSE "$PKG/"
  [ -f CREDITS.md ] && cp CREDITS.md "$PKG/"
  [ -f summarize.sh ] && cp summarize.sh "$PKG/"
  # Optional reviewed persona extension. The native offline catalog remains
  # authoritative when this file is absent.
  [ -f personas.tsv ] && cp personas.tsv "$PKG/"

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

  if [ -e prebuilt/resetprop-rs ] || [ -e prebuilt/resetprop-rs.sha256 ] ||
     [ -e prebuilt/resetprop-rs.LICENSE ]; then
    if [ ! -f prebuilt/resetprop-rs ] || [ ! -f prebuilt/resetprop-rs.sha256 ] ||
       [ ! -f prebuilt/resetprop-rs.LICENSE ]; then
      echo "  ERROR: resetprop-rs binary/checksum/license set is incomplete" >&2
      exit 1
    fi
    if ! command -v sha256sum >/dev/null 2>&1; then
      echo "  ERROR: sha256sum is required to verify bundled resetprop-rs" >&2
      exit 1
    fi
    ( cd prebuilt && sha256sum -c resetprop-rs.sha256 >/dev/null ) || {
      echo "  ERROR: prebuilt/resetprop-rs checksum mismatch — refusing to package" >&2
      exit 1
    }
    echo "  ==> resetprop-rs verified"
    cp prebuilt/resetprop-rs prebuilt/resetprop-rs.sha256 \
      prebuilt/resetprop-rs.LICENSE "$PKG/bin/"
  else
    echo "  ==> resetprop-rs not bundled; runtime requires resetprop/resetprop-rs in PATH"
  fi

  bash tests/package_manifest_test.sh "$PKG"

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
