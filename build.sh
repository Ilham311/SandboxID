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

LSP_CMAKE=""
LSP_STATUS="disabled (SBX_ENABLE_LSPLANT=OFF requested)"
if [ "${SBX_ENABLE_LSPLANT:-ON}" = "ON" ]; then
  LSP_CMAKE="-DSBX_ENABLE_LSPLANT=ON"
  LSP_REV="$(grep -E '^LSPLANT_REF='  jni/fetch_lsplant_deps.sh 2>/dev/null | head -1 | cut -d= -f2 | tr -d '"' || true)"
  DOBBY_REV="$(grep -E '^DOBBY_REF='  jni/fetch_lsplant_deps.sh 2>/dev/null | head -1 | cut -d= -f2 | tr -d '"' || true)"
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
      $DBG_FLAG ${LSP_CMAKE:-} >/dev/null
    cmake --build "$BUILD" -j
  done

  rm -rf "$PKG"
  mkdir -p "$PKG/zygisk" "$PKG/bin"

  # Di repo, berkas ditata rapi per-folder (scripts/, data/). Namun framework
  # Magisk/KSU/APatch dan path absolut di jni/config.hpp mengharapkan semuanya
  # berada DATAR di root modul (/data/adb/modules/sandboxid/). Karena itu build.sh
  # "meratakan" kembali seluruh berkas ke root paket ($PKG/) di sini.

  # Manifest + lisensi (wajib; gagal keras bila hilang)
  cp module.prop "$PKG/"
  [ -f LICENSE ]    && cp LICENSE    "$PKG/"
  [ -f CREDITS.md ] && cp CREDITS.md "$PKG/"

  # Skrip lifecycle yang dipanggil framework (install, boot, tombol Action)
  cp scripts/lifecycle/customize.sh "$PKG/"
  cp scripts/lifecycle/service.sh   "$PKG/"
  cp scripts/lifecycle/action.sh    "$PKG/"
  [ -f scripts/lifecycle/post-fs-data.sh ] && cp scripts/lifecycle/post-fs-data.sh "$PKG/"

  # Pustaka bersama + skrip identitas + diagnostik
  [ -f scripts/lib/helpers.sh ]         && cp scripts/lib/helpers.sh         "$PKG/"
  [ -f scripts/identity/rotate_ids.sh ] && cp scripts/identity/rotate_ids.sh "$PKG/"
  [ -f scripts/identity/autopif.sh ]    && cp scripts/identity/autopif.sh    "$PKG/"
  [ -f scripts/debug/summarize.sh ]     && cp scripts/debug/summarize.sh     "$PKG/"
  [ -f scripts/debug/selftest.sh ]      && cp scripts/debug/selftest.sh      "$PKG/"

  # Data referensi + konfigurasi pengguna
  [ -f data/personas.tsv ] && cp data/personas.tsv "$PKG/"
  [ -f data/devices.tsv ]  && cp data/devices.tsv  "$PKG/"
  [ -f data/carriers.tsv ] && cp data/carriers.tsv "$PKG/"
  [ -f data/target.txt ]   && cp data/target.txt   "$PKG/"

  # WebUI (disalin utuh)
  [ -d webroot ] && cp -R webroot "$PKG/"

  # Shim PATH: `su -c sandboxid ...` gagal karena bin/ tak ada di PATH. Berkas di
  # system/bin/ di-magic-mount ke /system/bin (universal Magisk/KSU/APatch), lalu
  # meng-exec binary per-ABI di bin/. Memperbaiki "su -c sandboxid tidak berkerja".
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
