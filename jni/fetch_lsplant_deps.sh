#!/usr/bin/env bash
# fetch_lsplant_deps.sh — vendor the L3 (LSPlant + Dobby + lsparself) sources
# into jni/external/ so a build with -DSBX_ENABLE_LSPLANT=ON can compile.
#
# jni/external/ is intentionally git-ignored (see .gitignore): these are large
# third-party trees, fetched on demand and never committed. The destination
# paths below MUST match the ones hard-coded in jni/CMakeLists.txt:
#     external/lsplant/lsplant/src/main/jni   (add_subdirectory -> lsplant_static)
#     external/dobby                          (add_subdirectory -> dobby)
#     external/lsparself/lsparself.hpp        (header-only; libart symbol resolver)
#
# Overridable pins (env):
#   LSPLANT_REPO / LSPLANT_REF   default: LSPosed/LSPlant @ v2.0
#   DOBBY_REPO   / DOBBY_REF     default: jmpews/Dobby   @ 5dfc854 (pinned commit)
#                                (override to pin a different reviewed commit)
#   LSPARSELF_HPP                path to a lsparself.hpp to copy in (see below)
#                                REQUIRED unless the LSPlant checkout already
#                                ships one: lsparself has no public repo, so
#                                there is no default source for this file.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"   # jni/
EXT="$SCRIPT_DIR/external"

LSPLANT_REPO="${LSPLANT_REPO:-https://github.com/LSPosed/LSPlant.git}"
LSPLANT_REF="${LSPLANT_REF:-v2.0}"
DOBBY_REPO="${DOBBY_REPO:-https://github.com/jmpews/Dobby.git}"
# Pin to an exact reviewed commit for reproducible / auditable builds.
DOBBY_REF="${DOBBY_REF:-5dfc8546954ce3b3198132ab13fddb89ee92cdd7}"

if ! command -v git >/dev/null 2>&1; then
  echo "ERROR: git is required" >&2
  exit 1
fi
mkdir -p "$EXT"

# clone_at <repo> <ref> <dest> — shallow, idempotent, submodules included.
clone_at() {
  local repo="$1" ref="$2" dest="$3"
  if [ -d "$dest/.git" ]; then
    echo "==> $dest already present; leaving as-is (rm -rf to refetch)"
    return 0
  fi
  echo "==> cloning $repo @ $ref -> $dest"
  rm -rf "$dest"
  # Shallow clone at an exact tag/branch; fall back to a full clone + checkout
  # so an arbitrary commit SHA in *_REF still works.
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

clone_at "$LSPLANT_REPO" "$LSPLANT_REF" "$EXT/lsplant"
clone_at "$DOBBY_REPO"   "$DOBBY_REF"   "$EXT/dobby"

# --- sanity: the exact paths jni/CMakeLists.txt references must now exist ----
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

# --- lsparself.hpp (libart.so symbol resolver used by sbx_lsplant.hpp) -------
# sbx_lsplant.hpp does:  lsparself::Elf art("/libart.so");
#                        info.art_symbol_resolver = [&](sv){ art.getSymbAddress(sv); ... }
# lsparself is LSPosed's private (not publicly released) libart.so symbol
# parser — there is no public repo to clone it from, so unlike LSPlant/Dobby
# this script has NO default source for it. We never substitute a look-alike
# header (a wrong-API one would only fail to compile later); we reuse an exact
# match if the LSPlant checkout happens to ship one, else the caller MUST pass
# LSPARSELF_HPP=/path/to/lsparself.hpp, or we stop with an actionable message.
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
  else
    {
      echo "ERROR: lsparself.hpp not found."
      echo "  jni/sbx_lsplant.hpp needs <lsparself.hpp> providing lsparself::Elf with"
      echo "  getSymbAddress()/getSymbPrefixFirstAddress() to resolve libart.so symbols."
      echo "  Supply it once with:"
      echo "      LSPARSELF_HPP=/path/to/lsparself.hpp $0"
      echo "  (it is then cached at $EXT/lsparself/lsparself.hpp)."
    } >&2
    exit 1
  fi
fi

echo "==> L3 dependencies ready under $EXT"
