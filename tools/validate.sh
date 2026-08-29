#!/usr/bin/env bash
set -u
# validate.sh berada di tools/, jadi naik satu tingkat ke root repo agar semua
# path relatif di bawah (jni/, tests/, scripts/, data/) tetap konsisten.
cd "$(dirname "$0")/.." || exit 1
rc=0

echo "=== 1/4 clang++ -fsyntax-only (debug + release) ==="
for f in jni/main.cpp jni/companion.cpp jni/sandboxid.cpp; do
  if clang++ -std=c++20 -fsyntax-only -DSBX_DEBUG=1 -Wall -Wextra -Ijni "$f" 2>&1; then
    echo "OK(debug): $f"
  else
    echo "FAIL(debug): $f"; rc=1
  fi
  if clang++ -std=c++20 -fsyntax-only -Wall -Wextra -Ijni "$f" 2>&1; then
    echo "OK(rel):   $f"
  else
    echo "FAIL(rel):   $f"; rc=1
  fi
done

echo "=== 2/4 shell syntax (semua *.sh terlacak, ikut shebang) ==="
SBX_SH=$(git ls-files '*.sh' 2>/dev/null)
[ -n "$SBX_SH" ] || SBX_SH=$(find . -name '*.sh' -not -path './.git/*' -not -path './build/*' -not -path './out/*' -not -path './node_modules/*' -not -path './vendor/*' -not -path './third_party/*' | sed 's|^\./||')
for s in $SBX_SH; do
  if head -1 "$s" | grep -q bash; then
    if bash -n "$s" 2>&1; then echo "OK: bash -n $s"; else echo "FAIL: bash -n $s"; rc=1; fi
  else
    if sh -n "$s" 2>&1; then echo "OK: sh -n $s"; else echo "FAIL: sh -n $s"; rc=1; fi
  fi
done

echo "=== 3/4 host unit tests ==="
if clang++ -std=c++20 -o /tmp/sbx_carrier_test tests/carrier_test.cpp 2>&1 && /tmp/sbx_carrier_test; then
  echo "OK: carrier_test"
else
  echo "FAIL: carrier_test"; rc=1
fi
if clang++ -std=c++20 -o /tmp/sbx_native_read_test tests/native_read_test.cpp 2>&1 && /tmp/sbx_native_read_test; then
  echo "OK: native_read_test"
else
  echo "FAIL: native_read_test"; rc=1
fi

echo "=== 3b/4 autopif artifact well-formedness ==="
SBX_ART=/tmp/sbx_autopif_identity
if MODDIR="$PWD" AUTOPIF_DEVICES="$PWD/data/devices.tsv" AUTOPIF_ARTIFACT="$SBX_ART" \
     sh scripts/identity/autopif.sh device >/dev/null 2>&1 \
   && [ -s "$SBX_ART" ]; then
  bad=$(grep -vc '^[A-Za-z_][A-Za-z0-9_]*=[^=]*$' "$SBX_ART")
  glue=$(grep -c '^FLAVOR=.*=' "$SBX_ART")
  btu=$(grep -c '^BUILD_TIME_UTC=[0-9]*$' "$SBX_ART")
  if [ "$bad" = "0" ] && [ "$glue" = "0" ] && [ "$btu" -ge 1 ]; then
    echo "OK: autopif artifact well-formed ($(grep -c '=' "$SBX_ART") keys, no glued lines)"
  else
    echo "FAIL: autopif artifact malformed (bad-lines=$bad glued-FLAVOR=$glue BUILD_TIME_UTC-keys=$btu)"; rc=1
  fi
else
  echo "FAIL: autopif.sh device produced no artifact"; rc=1
fi

echo "=== 4/4 shellcheck (severity>=warning, semua *.sh — sama seperti CI) ==="
if command -v shellcheck >/dev/null 2>&1; then
  shellcheck --version | head -2
  if shellcheck -S warning $SBX_SH; then
    echo "OK: shellcheck clean"
  else
    echo "FAIL: shellcheck"; rc=1
  fi
else
  echo "SKIP: shellcheck not installed"
fi

echo "=== RESULT: $([ $rc -eq 0 ] && echo PASS || echo FAIL) ==="
exit $rc
