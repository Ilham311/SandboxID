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

# ----------------------------------------------------------------------------
# Patch tambahan Dobby untuk device W^X keras (Android 15/16; SELinux execmem
# ditolak / execmod non-uniform). Dua bug hulu Dobby yang memicu FORCE-CLOSE:
#
#  (1) OSMemory::SetPermission() memanggil FATAL()=abort() saat mprotect gagal.
#      Di lib hook yang disuntik ke proses asing, SATU mprotect exec yang ditolak
#      meng-ABORT seluruh proses target (force close). Diganti ERROR_LOG (tanpa
#      abort); fungsi tetap `return ret == 0` -> pemanggil membatalkan hook mulus.
#  (2) NearMemoryArena::AllocateChunk() mengabaikan nilai balik SetPermission lalu
#      memakai page yang gagal dijadikan exec -> lompatan ke page non-exec = SIGSEGV
#      (half-apply). Diperbaiki: cek nilai balik; bila gagal Free page & jatuh ke
#      pencarian island exec (execmod) yang SUDAH ada di bawahnya (ala ShadowHook).
#
# Referensi: Frida/ShadowHook tak pernah abort host saat proteksi ditolak; ART JIT
# & LSPlant menulis ke region exec file-backed (execmod), bukan anon execmem.
# Disuntik in-place via sed/awk (TANPA perl -- Termux tak punya perl). Idempotent
# + fail-loud bila layout hulu berubah dari pin DOBBY_REF.
# ----------------------------------------------------------------------------
DOBBY_POSIX="$EXT/dobby/source/UserMode/UnifiedInterface/platform-posix.cc"
DOBBY_NEARARENA="$EXT/dobby/source/MemoryAllocator/NearMemoryArena.cc"
for _sbxf in "$DOBBY_POSIX" "$DOBBY_NEARARENA"; do
  if [ ! -f "$_sbxf" ]; then
    echo "ERROR: target patch Dobby tidak ada: $_sbxf" >&2
    echo "  (layout Dobby berubah dari pin $DOBBY_REF; sesuaikan path/patch)" >&2
    exit 1
  fi
done

# (1) SetPermission: FATAL(abort) -> ERROR_LOG (tanpa abort)
if grep -qF "[SandboxID] OSMemory::SetPermission" "$DOBBY_POSIX"; then
  echo "==> Dobby SetPermission sudah dipatch (skip)"
else
  if ! grep -qE '^ *FATAL\("\[!\] %s.*strerror\(errno\)' "$DOBBY_POSIX"; then
    echo "ERROR: pola FATAL SetPermission tak ditemukan di $DOBBY_POSIX (layout hulu berubah)" >&2
    exit 1
  fi
  sed -i 's#^\( *\)FATAL("\[!\] %s.*strerror(errno).*;#\1ERROR_LOG("[SandboxID] OSMemory::SetPermission mprotect gagal (errno=%d) - hook dibatalkan, proses target TIDAK di-abort", errno);#' "$DOBBY_POSIX"
  grep -qF "[SandboxID] OSMemory::SetPermission" "$DOBBY_POSIX" || {
    echo "ERROR: gagal menerapkan patch SetPermission di $DOBBY_POSIX" >&2; exit 1; }
  echo "==> patched Dobby SetPermission (FATAL->ERROR_LOG, tanpa abort) -> $DOBBY_POSIX"
fi

# (2) NearMemoryArena::AllocateChunk: cek nilai balik SetPermission + fallthrough island
if grep -qF "jatuh ke pencarian island exec" "$DOBBY_NEARARENA"; then
  echo "==> Dobby NearMemoryArena sudah dipatch (skip)"
else
  if ! grep -qE '^    OSMemory::SetPermission\(\(void \*\)blank_page_addr' "$DOBBY_NEARARENA"; then
    echo "ERROR: pola AllocateChunk blank_page tak ditemukan di $DOBBY_NEARARENA (layout hulu berubah)" >&2
    exit 1
  fi
  awk '
  /^    OSMemory::SetPermission\(\(void \*\)blank_page_addr, OSMemory::PageSize\(\), permission\);$/ {
      print "    // SandboxID: SetPermission (mprotect exec) bisa ditolak di device W^X. Cek";
      print "    // nilai baliknya; bila gagal lepaskan page & JANGAN dipakai (cegah lompatan";
      print "    // ke page non-exec = SIGSEGV), jatuh ke pencarian island exec (execmod).";
      print "    if (OSMemory::SetPermission((void *)blank_page_addr, OSMemory::PageSize(), permission)) {";
      print "      NearMemoryArena::PushPage(blank_page_addr, permission);";
      print "      goto search_once_more;";
      print "    }";
      print "    OSMemory::Free((void *)blank_page_addr, OSMemory::PageSize());";
      skip = 2; next;
  }
  skip > 0 { skip--; next; }
  { print }
  ' "$DOBBY_NEARARENA" > "$DOBBY_NEARARENA.sbxtmp" && mv "$DOBBY_NEARARENA.sbxtmp" "$DOBBY_NEARARENA"
  grep -qF "jatuh ke pencarian island exec" "$DOBBY_NEARARENA" || {
    echo "ERROR: gagal menerapkan patch NearMemoryArena di $DOBBY_NEARARENA" >&2; exit 1; }
  echo "==> patched Dobby NearMemoryArena (cek SetPermission + island fallthrough) -> $DOBBY_NEARARENA"
fi

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
