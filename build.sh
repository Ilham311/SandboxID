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
VERSION="$(grep '^version=' module.prop | cut -d= -f2 || true)"
if [ -z "${VERSION:-}" ]; then
  echo "ERROR: version= line missing in module.prop" >&2
  exit 1
fi
MODULE_ID="$(grep '^id=' module.prop | cut -d= -f2 || true)"
if [ -z "${MODULE_ID:-}" ]; then
  echo "ERROR: id= line missing in module.prop" >&2
  exit 1
fi

LSP_CMAKE=""
LSP_STATUS="disabled (SBX_ENABLE_LSPLANT=OFF requested)"
if [ "${SBX_ENABLE_LSPLANT:-ON}" = "ON" ]; then
  LSP_CMAKE="-DSBX_ENABLE_LSPLANT=ON"
  _refval() { sed -n "s/^$1=//p" jni/fetch_lsplant_deps.sh 2>/dev/null | head -1 \
                | sed -e 's/^"//' -e 's/"$//' -e 's/^\${[^:]*:-//' -e 's/}$//'; }
  LSP_REV="$(_refval LSPLANT_REF || true)"
  DOBBY_REV="$(_refval DOBBY_REF || true)"
  LSP_STATUS="enabled [LSPlant=${LSP_REV:-?} Dobby=${DOBBY_REV:-?}]"
  echo "==> L3 LSPlant $LSP_STATUS — preparing dependencies + callback DEX"
  bash "$ROOT/jni/fetch_lsplant_deps.sh"
  if ! bash "$ROOT/jni/tools/gen_hook_dex.sh"; then
    echo "  WARN: hook_dex.h generation failed — L3 ANDROID_ID hook will be skipped" >&2
    echo "        at runtime (install a JDK + Android SDK build-tools to enable it)" >&2
  fi
fi
OUT="$ROOT/dist"

ABIS=(arm64-v8a armeabi-v7a x86_64 x86)

echo "==> SandboxID $VERSION"
echo "==> NDK:        $ANDROID_NDK_HOME"
echo "==> MIN_SDK:    $MIN_SDK"
echo "==> Variant(s): $VARIANT"
echo "==> LSPlant:    $LSP_STATUS"

bash "$ROOT/jni/tools/fetch_zygisk_hpp.sh"

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
      -DSBX_MODULE_ID="$MODULE_ID" \
      $DBG_FLAG ${LSP_CMAKE:-} >/dev/null
    cmake --build "$BUILD" -j
  done

  rm -rf "$PKG"
  mkdir -p "$PKG/zygisk" "$PKG/bin"

  cp module.prop "$PKG/"
  [ -f LICENSE ]    && cp LICENSE    "$PKG/"
  [ -f CREDITS.md ] && cp CREDITS.md "$PKG/"

  cp scripts/lifecycle/customize.sh "$PKG/"
  cp scripts/lifecycle/service.sh   "$PKG/"
  cp scripts/lifecycle/action.sh    "$PKG/"
  [ -f scripts/lifecycle/post-fs-data.sh ] && cp scripts/lifecycle/post-fs-data.sh "$PKG/"

  [ -f scripts/lib/helpers.sh ]         && cp scripts/lib/helpers.sh         "$PKG/"
  [ -f scripts/identity/rotate_ids.sh ] && cp scripts/identity/rotate_ids.sh "$PKG/"
  [ -f scripts/identity/autopif.sh ]    && cp scripts/identity/autopif.sh    "$PKG/"
  [ -f scripts/debug/summarize.sh ]     && cp scripts/debug/summarize.sh     "$PKG/"
  [ -f scripts/debug/selftest.sh ]      && cp scripts/debug/selftest.sh      "$PKG/"

  [ -f data/personas.tsv ] && cp data/personas.tsv "$PKG/"
  [ -f data/devices.tsv ]  && cp data/devices.tsv  "$PKG/"
  [ -f data/carriers.tsv ] && cp data/carriers.tsv "$PKG/"
  [ -f data/target.txt ]   && cp data/target.txt   "$PKG/"

  [ -d webroot ] && cp -R webroot "$PKG/"

  mkdir -p "$PKG/system/bin"
  cat > "$PKG/system/bin/sandboxid" <<'WRAP'
#!/system/bin/sh
d=/data/adb/modules/sandboxid/bin
if [ -x "$d/sandboxid" ]; then exec "$d/sandboxid" "$@"; fi
case "$(getprop ro.product.cpu.abi)" in
  arm64*)   exec "$d/sandboxid-arm64"  "$@" ;;
  armeabi*) exec "$d/sandboxid-arm"    "$@" ;;
  x86_64)   exec "$d/sandboxid-x86_64" "$@" ;;
  x86)      exec "$d/sandboxid-x86"    "$@" ;;
esac
echo "sandboxid: tak ada binary untuk ABI $(getprop ro.product.cpu.abi)" >&2
exit 127
WRAP
  chmod 0755 "$PKG/system/bin/sandboxid"

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
