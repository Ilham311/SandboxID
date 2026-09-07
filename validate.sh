#!/usr/bin/env bash
set -u
cd "$(dirname "$0")" || exit 1
rc=0

SBX_TMP_ROOT="${SBX_VALIDATE_TMPDIR:-${TMPDIR:-$PWD/.claude-tmp}}"
if ! mkdir -p "$SBX_TMP_ROOT" 2>/dev/null || [ ! -w "$SBX_TMP_ROOT" ]; then
  echo "FAIL: validation temp directory is not writable: $SBX_TMP_ROOT"
  exit 1
fi
SBX_TMP=$(mktemp -d "$SBX_TMP_ROOT/sbx-validate.XXXXXX") || exit 1
trap 'rm -rf "$SBX_TMP"' EXIT

echo "=== 1/6 clang++ -fsyntax-only (debug + release) ==="
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

echo "=== 2/6 shell + WebUI JavaScript syntax ==="
SBX_SH=$(git ls-files '*.sh' 2>/dev/null)
[ -n "$SBX_SH" ] || SBX_SH=$(find . -name '*.sh' -not -path './.git/*' -not -path './build/*' -not -path './out/*' -not -path './node_modules/*' -not -path './vendor/*' -not -path './third_party/*' | sed 's|^\./||')
for s in $SBX_SH; do
  if head -1 "$s" | grep -q bash; then
    if bash -n "$s" 2>&1; then echo "OK: bash -n $s"; else echo "FAIL: bash -n $s"; rc=1; fi
  else
    if sh -n "$s" 2>&1; then echo "OK: sh -n $s"; else echo "FAIL: sh -n $s"; rc=1; fi
  fi
done
if command -v node >/dev/null 2>&1; then
  for js in webroot/theme-init.js webroot/app.js; do
    if node --check "$js" 2>&1; then
      echo "OK: node --check $js"
    else
      echo "FAIL: node --check $js"; rc=1
    fi
  done
else
  echo "SKIP: node not installed"
fi

echo "=== 3/6 host unit tests ==="
if clang++ -std=c++20 -o "$SBX_TMP/sbx_carrier_test" tests/carrier_test.cpp 2>&1 \
   && "$SBX_TMP/sbx_carrier_test"; then
  echo "OK: carrier_test"
else
  echo "FAIL: carrier_test"; rc=1
fi
if clang++ -std=c++20 -o "$SBX_TMP/sbx_native_read_test" tests/native_read_test.cpp 2>&1 \
   && "$SBX_TMP/sbx_native_read_test"; then
  echo "OK: native_read_test"
else
  echo "FAIL: native_read_test"; rc=1
fi
if command -v python3 >/dev/null 2>&1 \
   && python3 tests/probe_expectations_test.py; then
  echo "OK: probe_expectations_test"
else
  echo "FAIL: probe_expectations_test"; rc=1
fi

SBX_ZERO_GAID=00000000-0000-0000-0000-000000000000
SBX_GAID_TEST="$SBX_TMP/gaid-optout"
mkdir -p "$SBX_GAID_TEST/mod" "$SBX_GAID_TEST/bin"
cp helpers.sh rotate_ids.sh "$SBX_GAID_TEST/mod/"
printf 'version=test\n' > "$SBX_GAID_TEST/mod/module.prop"
printf 'GOOGLE_AID=%s\n' "$SBX_ZERO_GAID" > "$SBX_GAID_TEST/mod/identity.prop"
printf '#!/bin/sh\nprintf "%%s\\n" "$*" >> "$SBX_GAID_CALLS"\n' > "$SBX_GAID_TEST/bin/settings"
printf '#!/bin/sh\nexit 0\n' > "$SBX_GAID_TEST/bin/am"
chmod 0755 "$SBX_GAID_TEST/bin/settings" "$SBX_GAID_TEST/bin/am"
if PATH="$SBX_GAID_TEST/bin:$PATH" MODDIR="$SBX_GAID_TEST/mod" \
   LOGFILE="$SBX_GAID_TEST/rotate.log" BACKUP_DIR_ROOT="$SBX_GAID_TEST/backups" \
   SBX_GAID_CALLS="$SBX_GAID_TEST/calls" \
   sh "$SBX_GAID_TEST/mod/rotate_ids.sh" gaid "$SBX_ZERO_GAID" >/dev/null 2>&1 \
   && grep -q 'put --user 0 global advertising_id 00000000-0000-0000-0000-000000000000' "$SBX_GAID_TEST/calls" \
   && grep -q 'put --user 0 global limit_ad_tracking 1' "$SBX_GAID_TEST/calls"; then
  echo "OK: all-zero GAID preserves local opt-out state"
else
  echo "FAIL: all-zero GAID opt-out semantics"; rc=1
fi

echo "=== 4/6 autopif exact-SDK artifact ==="
SBX_ART="$SBX_TMP/sbx_autopif_identity"
if MODDIR="$PWD" SBX_REAL_SDK=35 SBX_REAL_RELEASE=15 \
   AUTOPIF_ARTIFACT="$SBX_ART" sh autopif.sh device >/dev/null 2>&1 \
   && [ -s "$SBX_ART" ]; then
  bad=$(grep -vc '^[A-Za-z_][A-Za-z0-9_]*=[^=]*$' "$SBX_ART")
  glue=$(grep -c '^FLAVOR=.*=' "$SBX_ART")
  btu=$(grep -c '^BUILD_TIME_UTC=[0-9]*$' "$SBX_ART")
  if [ "$bad" = "0" ] && [ "$glue" = "0" ] && [ "$btu" -ge 1 ] \
     && grep -q '^SDK_INT=35$' "$SBX_ART"; then
    echo "OK: autopif exact-SDK artifact ($(grep -c '=' "$SBX_ART") keys, no glued lines)"
  else
    echo "FAIL: autopif artifact malformed or wrong SDK (bad-lines=$bad glued-FLAVOR=$glue BUILD_TIME_UTC-keys=$btu)"; rc=1
  fi
else
  echo "FAIL: autopif.sh device produced no exact-SDK artifact"; rc=1
fi

if MODDIR="$PWD" SBX_REAL_SDK=32 SBX_REAL_RELEASE=12 \
   AUTOPIF_ARTIFACT="$SBX_TMP/unsupported_identity" \
   sh autopif.sh device >"$SBX_TMP/unsupported.log" 2>&1; then
  echo "FAIL: autopif accepted an SDK absent from devices.tsv"; rc=1
elif grep -q 'tidak ada persona dengan SDK runtime 32' "$SBX_TMP/unsupported.log"; then
  echo "OK: autopif rejects an SDK absent from devices.tsv"
else
  echo "FAIL: autopif exact-SDK rejection was not actionable"; rc=1
fi

echo "=== 5/6 repository diff check ==="
if git diff --check; then
  echo "OK: git diff --check"
else
  echo "FAIL: git diff --check"; rc=1
fi

echo "=== 6/6 shellcheck (severity>=warning, semua *.sh — sama seperti CI) ==="
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
