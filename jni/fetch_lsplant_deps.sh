#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
EXT="$SCRIPT_DIR/external"

LSPLANT_REPO="${LSPLANT_REPO:-https://github.com/LSPosed/LSPlant.git}"
LSPLANT_REF="${LSPLANT_REF:-v2.0}"
DOBBY_REPO="${DOBBY_REPO:-https://github.com/jmpews/Dobby.git}"
DOBBY_REF="${DOBBY_REF:-5dfc8546954ce3b3198132ab13fddb89ee92cdd7}"

if ! command -v git >/dev/null 2>&1; then
  echo "ERROR: git is required" >&2
  exit 1
fi
mkdir -p "$EXT"

clone_at() {
  local repo="$1" ref="$2" dest="$3"
  if [ -d "$dest/.git" ]; then
    echo "==> $dest already present; leaving as-is (rm -rf to refetch)"
    return 0
  fi
  echo "==> cloning $repo @ $ref -> $dest"
  rm -rf "$dest"
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

# ---- patch vendored Dobby so it builds for ELF/Android arm64 with clang ----
# The pinned Dobby commit has two Android/arm64 build breakers. Fix them in the
# clone (we vendor by clone, not by fork). Both guards make this idempotent, so
# re-running against an already-present clone is a no-op.
patch_dobby() {
  local plat="$EXT/dobby/source/PlatformUnifiedInterface/platform.h"
  local casm="$EXT/dobby/source/TrampolineBridge/ClosureTrampolineBridge/arm64/closure_bridge_arm64.asm"

  # (1) Break the platform.h <-> dobby/common.h #include cycle. Under #pragma once,
  #     when platform.h is the entry TU the nested common/os_arch_features.h is
  #     reached before platform.h finishes, so its make_memory_readable() sees an
  #     as-yet-undeclared OSMemory -> "use of undeclared identifier 'OSMemory'".
  #     platform.h itself only needs the standard size/int/va_list types, plus
  #     dobby.h (addr_t) and dobby/platform_features.h (the TINYSTL "stl" alias
  #     used pervasively by ProcessRuntime.* / dobby_symbol_resolver.cc) — pull
  #     those directly instead of the umbrella header; the cycle then disappears
  #     while addr_t/stl stay visible.
  if [ -f "$plat" ] && grep -q '#include "dobby/common.h"' "$plat"; then
    sed -i 's|#include "dobby/common.h"|#include <cstddef>\n#include <cstdint>\n#include <cstdarg>\n#include "dobby.h"\n#include "dobby/platform_features.h"|' "$plat"
    echo "==> patched Dobby platform.h (broke common.h include cycle -> OSMemory/addr_t/stl visible)"
  fi

  # (2) The arm64 closure-bridge trampoline emits Mach-O @PAGE/@PAGEOFF relocation
  #     syntax unconditionally; the ELF assembler rejects it and needs bare adrp +
  #     :lo12:. Keep the Apple form under __APPLE__, use the ELF form otherwise.
  if [ -f "$casm" ] && ! grep -q ':lo12:cdecl(common_closure_bridge_handler)' "$casm"; then
    awk '
      /adrp TMP_REG_0, cdecl\(common_closure_bridge_handler\)@PAGE/ {
        pending = 1; next
      }
      pending == 1 {
        pending = 0
        if ($0 ~ /add TMP_REG_0, TMP_REG_0, cdecl\(common_closure_bridge_handler\)@PAGEOFF/) {
          print "#if defined(__APPLE__)"
          print "adrp TMP_REG_0, cdecl(common_closure_bridge_handler)@PAGE"
          print "add TMP_REG_0, TMP_REG_0, cdecl(common_closure_bridge_handler)@PAGEOFF"
          print "#else"
          print "adrp TMP_REG_0, cdecl(common_closure_bridge_handler)"
          print "add TMP_REG_0, TMP_REG_0, :lo12:cdecl(common_closure_bridge_handler)"
          print "#endif"
          next
        } else {
          print "adrp TMP_REG_0, cdecl(common_closure_bridge_handler)@PAGE"
          print
          next
        }
      }
      { print }
    ' "$casm" > "$casm.sbxtmp" && mv "$casm.sbxtmp" "$casm"
    echo "==> patched Dobby closure_bridge_arm64.asm (@PAGE/@PAGEOFF -> ELF adrp + :lo12:)"
  fi

  # (3) dobby_symbol_resolver.cc references module.load_address, but
  #     RuntimeModule (source/PlatformUtil/ProcessRuntime.h) only declares a
  #     `base` field -> "no member named 'load_address' in 'RuntimeModule'".
  #     Use the field that actually exists.
  local resolver="$EXT/dobby/builtin-plugin/SymbolResolver/elf/dobby_symbol_resolver.cc"
  if [ -f "$resolver" ] && grep -q 'module\.load_address' "$resolver"; then
    sed -i 's/module\.load_address/module.base/g' "$resolver"
    echo "==> patched Dobby dobby_symbol_resolver.cc (module.load_address -> module.base)"
  fi

  # (4) ProcessRuntime.cc (source/Backend/UserMode/PlatformUtil/Linux) has the
  #     same load_address/RuntimeModule mismatch as (3), plus two build breakers
  #     of its own under the NDK clang toolchain:
  #       - memory_region_comparator compares `a.start`/`b.start`, but MemRange
  #         only declares `start()` as a method (see MemoryAllocator.h) -> "ref
  #         to non-static member function must be called".
  #       - the /proc/self/maps sscanf format string uses PRIxPTR without ever
  #         including <cinttypes>/<inttypes.h>, so the macro is left
  #         unexpanded and the adjacent string literals fail to concatenate ->
  #         "expected ')'".
  #     Fix the member reference, add the missing header, and reuse the
  #     module.base rename from (3).
  local runtime="$EXT/dobby/source/Backend/UserMode/PlatformUtil/Linux/ProcessRuntime.cc"
  if [ -f "$runtime" ]; then
    if grep -q 'module\.load_address' "$runtime"; then
      sed -i 's/module\.load_address/module.base/g' "$runtime"
      echo "==> patched Dobby ProcessRuntime.cc (module.load_address -> module.base)"
    fi
    if grep -q 'return (a\.start < b\.start);' "$runtime"; then
      sed -i 's/return (a\.start < b\.start);/return (a.start() < b.start());/' "$runtime"
      echo "==> patched Dobby ProcessRuntime.cc (a.start/b.start -> a.start()/b.start() calls)"
    fi
    if ! grep -q '#include <cinttypes>' "$runtime"; then
      awk '
        !done && /^#include <elf\.h>$/ {
          print "#include <cinttypes>"
          done = 1
        }
        { print }
      ' "$runtime" > "$runtime.sbxtmp" && mv "$runtime.sbxtmp" "$runtime"
      echo "==> patched Dobby ProcessRuntime.cc (added missing <cinttypes> for PRIxPTR)"
    fi
  fi
}
patch_dobby

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
