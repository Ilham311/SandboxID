#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
EXT="$SCRIPT_DIR/external"

LSPLANT_REPO="${LSPLANT_REPO:-https://github.com/LSPosed/LSPlant.git}"
LSPLANT_REF="${LSPLANT_REF:-v6.4}"
DOBBY_REPO="${DOBBY_REPO:-https://github.com/LSPosed/Dobby.git}"
DOBBY_REF="${DOBBY_REF:-edb2af1216313cf6c0d6771be2b279c1db573faf}"
XZ_REPO="${XZ_REPO:-https://github.com/tukaani-project/xz-embedded.git}"
XZ_REF="${XZ_REF:-master}"

if ! command -v git >/dev/null 2>&1; then
  echo "ERROR: git is required" >&2
  exit 1
fi
mkdir -p "$EXT"

# clone_at repo ref dest [submodule_path ...]
# Tanpa arg submodule: --recurse-submodules penuh (perilaku lama; dobby/xz).
# Dengan arg submodule: clone TANPA recurse, lalu init HANYA path yang diminta.
# Wajib untuk LSPlant v6.4 — .gitmodules-nya punya submodule test-only
# (test/.../lsparself, test/.../lsprism) berURL SSH git@github.com yang GAGAL di
# CI (tanpa kunci SSH). Build cuma butuh dex_builder (HTTPS), yang recursively
# menarik parallel_hashmap (HTTPS). depth di-drop pada submodule: SHA yang
# di-pin bisa bukan tip branch, jadi shallow fetch gagal.
clone_at() {
  local repo="$1" ref="$2" dest="$3"
  shift 3
  local submods=("$@")
  if [ -d "$dest/.git" ]; then
    echo "==> $dest already present; leaving as-is (rm -rf to refetch)"
    return 0
  fi
  echo "==> cloning $repo @ $ref -> $dest"
  rm -rf "$dest"

  if [ "${#submods[@]}" -gt 0 ]; then
    if ! git clone --depth 1 --branch "$ref" "$repo" "$dest" 2>/dev/null; then
      echo "   (shallow --branch '$ref' failed; retrying with full clone + checkout)"
      rm -rf "$dest"
      git clone "$repo" "$dest"
      git -C "$dest" checkout "$ref"
    fi
    local sm
    for sm in "${submods[@]}"; do
      echo "   -> init submodule: $sm"
      git -C "$dest" submodule update --init --recursive -- "$sm"
    done
    return 0
  fi

  if git clone --depth 1 --branch "$ref" --recurse-submodules --shallow-submodules \
        "$repo" "$dest" 2>/dev/null; then
    return 0
  fi
  echo "   (shallow --branch '$ref' failed; retrying with full clone + checkout)"
  rm -rf "$dest"
  git clone --recurse-submodules "$repo" "$dest"
  git -C "$dest" checkout "$ref"
  git -C "$dest" submodule update --init --recursive
}

clone_at "$LSPLANT_REPO" "$LSPLANT_REF" "$EXT/lsplant" \
         "lsplant/src/main/jni/external/dex_builder"
clone_at "$DOBBY_REPO"   "$DOBBY_REF"   "$EXT/dobby"
clone_at "$XZ_REPO"      "$XZ_REF"      "$EXT/xz"

LSP_JNI="$EXT/lsplant/lsplant/src/main/jni"
if [ ! -f "$LSP_JNI/CMakeLists.txt" ] || [ ! -f "$LSP_JNI/include/lsplant.hpp" ]; then
  {
    echo "ERROR: unexpected LSPlant layout — missing:"
    echo "         $LSP_JNI/CMakeLists.txt"
    echo "         $LSP_JNI/include/lsplant.hpp"
    echo "  The LSPlant repo layout may differ at ref '$LSPLANT_REF';"
    echo "  adjust jni/CMakeLists.txt paths or pin a compatible LSPLANT_REF."
  } >&2
  exit 1
fi
if [ ! -f "$EXT/dobby/include/dobby.h" ]; then
  echo "ERROR: unexpected Dobby layout — missing $EXT/dobby/include/dobby.h" >&2
  exit 1
fi

# Patch Dobby agar CodePatch() ramah W^X (Android 15 / API 35).
# Dobby yang di-pin (2021) menulis kode via mprotect(RWX)+memcpy; di Android 15
# kernel menolak halaman jadi writable+executable (W^X) sehingga memcpy menabrak
# halaman R-X -> SIGSEGV SEGV_ACCERR saat lsplant::Init memasang hook. Versi
# pengganti menulis lewat /proc/self/mem (pwrite64) yang menembus proteksi
# halaman tanpa RWX. external/ di-gitignore + CI meng-clone Dobby dari awal,
# jadi patch WAJIB disuntik di sini (edit working copy saja tak sampai ke CI).
DOBBY_CODEPATCH="$EXT/dobby/source/UserMode/ExecMemory/code-patch-tool-posix.cc"
DOBBY_CODEPATCH_FIX="$SCRIPT_DIR/patches/dobby_code_patch_tool_posix.cc"
if [ ! -f "$DOBBY_CODEPATCH" ]; then
  echo "ERROR: target patch Dobby tidak ada: $DOBBY_CODEPATCH" >&2
  echo "  (layout Dobby berubah dari pin $DOBBY_REF; sesuaikan path patch)" >&2
  exit 1
fi
if [ ! -f "$DOBBY_CODEPATCH_FIX" ]; then
  echo "ERROR: file pengganti tidak ada: $DOBBY_CODEPATCH_FIX" >&2
  exit 1
fi
cp "$DOBBY_CODEPATCH_FIX" "$DOBBY_CODEPATCH"
echo "==> patched Dobby CodePatch (W^X /proc/self/mem) -> $DOBBY_CODEPATCH"
if [ ! -f "$EXT/xz/linux/lib/xz/xz_dec_stream.c" ] \
   || [ ! -f "$EXT/xz/linux/include/linux/xz.h" ] \
   || [ ! -f "$EXT/xz/userspace/xz_config.h" ]; then
  {
    echo "ERROR: unexpected xz-embedded layout under $EXT/xz — expected:"
    echo "         linux/lib/xz/xz_dec_stream.c"
    echo "         linux/include/linux/xz.h"
    echo "         userspace/xz_config.h"
    echo "  The repo layout may differ at ref '$XZ_REF'; pin a compatible XZ_REF."
  } >&2
  exit 1
fi

mkdir -p "$EXT/lsparself"
if [ -f "$EXT/lsparself/lsparself.hpp" ]; then
  echo "==> lsparself.hpp already present"
elif [ -n "${LSPARSELF_HPP:-}" ] && [ -f "${LSPARSELF_HPP}" ]; then
  cp "${LSPARSELF_HPP}" "$EXT/lsparself/lsparself.hpp"
  echo "==> copied lsparself.hpp from \$LSPARSELF_HPP"
else
  found="$(find "$EXT/lsplant" -name 'lsparself.hpp' 2>/dev/null | head -1 || true)"
  if [ -n "$found" ]; then
    cp "$found" "$EXT/lsparself/lsparself.hpp"
    echo "==> reused $found"
  elif [ -f "$SCRIPT_DIR/lsparself.hpp" ]; then
    cp "$SCRIPT_DIR/lsparself.hpp" "$EXT/lsparself/lsparself.hpp"
    echo "==> copied lsparself.hpp locally from $SCRIPT_DIR/lsparself.hpp"
  else
    {
      echo "ERROR: lsparself.hpp not found."
      echo "  jni/sbx_lsplant.hpp needs <lsparself.hpp> providing lsparself::Elf with"
      echo "  getSymbAddress()/getSymbPrefixFirstAddress() to resolve libart.so symbols."
      echo "  Supply it by placing it in the jni/ folder or with:"
      echo "      LSPARSELF_HPP=/path/to/lsparself.hpp $0"
      echo "  (it is then cached at $EXT/lsparself/lsparself.hpp)."
    } >&2
    exit 1
  fi
fi

echo "==> L3 dependencies ready under $EXT"
