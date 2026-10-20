#!/usr/bin/env bash
set -euo pipefail
#
# Fetch the pinned sources for the L3 (Java hook) layer.
#
# LSPlant and Dobby are BOTH built from source as static libraries and linked
# into libsandboxid.so. Nothing is downloaded prebuilt: a prebuilt .so would have
# to be packaged beside the module and dlopen()ed from /data/adb/modules, which
# the app process cannot read (SELinux), and Zygisk only loads zygisk/<abi>.so.
#
# Pins (do not float these):
#   LSPlant  v6.4   7217ac6f41e2bda549e4acb54e632abb03e5ccaf
#   Dobby (LSPosed fork) c1da0315d7a2069bde2ab432e45d4ab6df91237a
#
# Dobby is LSPosed's fork, not jmpews' upstream. Upstream is not usable here in
# either branch:
#
#   master  hard-codes Mach-O relocations in closure_bridge_arm64.asm
#           (`sym@PAGE` / `sym@PAGEOFF`), which the ELF assembler rejects:
#             error: unexpected token in argument list
#   dev     the 2023 "Refactor? Yes" series deleted source/core/arch/Cpu.h and
#           the MemoryAllocator/CodeBuffer/ headers, but left the arm32 and
#           x86 code that includes them and the arm_inst_t typedefs they
#           defined. Every armeabi-v7a TU then fails with
#             'core/arch/Cpu.h' file not found
#             unknown type name 'arm_inst_t'
#           x86 and x86_64 happen to still compile, so the breakage only
#           surfaces on the two 32-bit ABIs.
#
# LSPosed's fork is the tree LSPlant itself is developed against, still has the
# pre-refactor layout (CodeBuffer/ headers, Cpu.h), and has no .asm files in the
# Android branch at all — the arm64 closure bridge is emitted by the Assembler
# instead. Verified with a full static build with -DANDROID on the host, and by
# syntax-checking every compiled TU for all four ABIs.
#
# The three submodules LSPlant needs are pinned by SHA as well, because a
# submodule branch ("master") is not a pin: a fresh clone today and a fresh clone
# next month would compile different code.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
EXT="$SCRIPT_DIR/external"

LSPLANT_REPO="${LSPLANT_REPO:-https://github.com/LSPosed/LSPlant.git}"
LSPLANT_REF="${LSPLANT_REF:-7217ac6f41e2bda549e4acb54e632abb03e5ccaf}"
DOBBY_REPO="${DOBBY_REPO:-https://github.com/LSPosed/Dobby.git}"
DOBBY_REF="${DOBBY_REF:-c1da0315d7a2069bde2ab432e45d4ab6df91237a}"

DEXBUILDER_REPO="${DEXBUILDER_REPO:-https://github.com/LSPosed/DexBuilder.git}"
DEXBUILDER_REF="${DEXBUILDER_REF:-9d57844a301077abf4c29e061b2458c56a363c8f}"
PHMAP_REPO="${PHMAP_REPO:-https://github.com/greg7mdp/parallel-hashmap.git}"
PHMAP_REF="${PHMAP_REF:-65775fa09fecaa65d0b0022ab6bf091c0e509445}"

if ! command -v git >/dev/null 2>&1; then
  echo "ERROR: git is required" >&2
  exit 1
fi
mkdir -p "$EXT"

clone_at() {
  local repo="$1" ref="$2" dest="$3"
  if [ -d "$dest/.git" ] && [ -f "$dest/.git/HEAD" ]; then
    local have
    have="$(git -C "$dest" rev-parse HEAD 2>/dev/null || true)"
    if [ "$have" = "$ref" ]; then
      echo "==> $dest already at $ref"
      return 0
    fi
    echo "==> $dest present at ${have:-?}, want $ref — re-fetching"
  fi
  echo "==> cloning $repo @ $ref -> $dest"
  rm -rf "$dest"
  if git clone --depth 1 "$repo" "$dest" 2>/dev/null \
     && git -C "$dest" checkout -q "$ref" 2>/dev/null; then
    return 0
  fi
  echo "   (shallow clone of a bare SHA failed; retrying with a full clone)"
  rm -rf "$dest"
  git clone "$repo" "$dest"
  git -C "$dest" checkout -q "$ref"
}

clone_at "$LSPLANT_REPO"    "$LSPLANT_REF"    "$EXT/lsplant"
clone_at "$DOBBY_REPO"      "$DOBBY_REF"      "$EXT/dobby"
clone_at "$DEXBUILDER_REPO" "$DEXBUILDER_REF" "$EXT/lsplant/lsplant/src/main/jni/external/dex_builder"
clone_at "$PHMAP_REPO"      "$PHMAP_REF" \
  "$EXT/lsplant/lsplant/src/main/jni/external/dex_builder/external/parallel_hashmap"

LSP_JNI="$EXT/lsplant/lsplant/src/main/jni"
for f in "$LSP_JNI/CMakeLists.txt" \
         "$LSP_JNI/include/lsplant.hpp" \
         "$LSP_JNI/lsplant.cc" \
         "$LSP_JNI/external/dex_builder/CMakeLists.txt" \
         "$LSP_JNI/external/dex_builder/external/parallel_hashmap/parallel_hashmap/phmap.h" \
         "$EXT/dobby/include/dobby.h" \
         "$EXT/dobby/CMakeLists.txt"; do
  if [ ! -f "$f" ]; then
    echo "ERROR: expected file missing after fetch: $f" >&2
    exit 1
  fi
done

# The fork must still be the pre-refactor tree: the arm32 and x86 code includes
# these headers and the typedefs they define. Upstream's `dev` deleted them and
# left the users behind, which is why it is not used here. Fail loudly rather
# than discover it as a wall of "unknown type name 'arm_inst_t'" on two ABIs.
for f in "$EXT/dobby/source/core/arch/Cpu.h" \
         "$EXT/dobby/source/MemoryAllocator/CodeBuffer/code_buffer_arm.h"; do
  if [ ! -f "$f" ]; then
    echo "ERROR: DOBBY_REF=$DOBBY_REF is missing $f" >&2
    echo "       That tree has the post-2023 refactor and cannot build armeabi-v7a." >&2
    exit 1
  fi
done

# LSPlant v6.4 is the last tag before the C++23 modules rework. Guard against a
# silently different LSPLANT_REF being passed in: the module's CMake assumes the
# plain single-TU build (no .ixx units, no find_package(cxx)).
if find "$LSP_JNI" -name '*.ixx' -print -quit 2>/dev/null | grep -q .; then
  echo "ERROR: LSPLANT_REF=$LSPLANT_REF is a C++23-modules tree (.ixx present)." >&2
  echo "       native/CMakeLists.txt expects the C++20 single-TU build (v6.4)." >&2
  exit 1
fi

echo "==> L3 dependencies ready under $EXT"
