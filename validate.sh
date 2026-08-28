#!/usr/bin/env bash
# One-shot validation for the Phase-4 edits: C++ syntax (debug+release),
# shell syntax, host unit tests, shellcheck. Read-only; writes only /tmp.
set -u
cd "$(dirname "$0")" || exit 1
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

echo "=== 2/4 shell syntax ==="
for s in autopif.sh selftest.sh; do
  if sh -n "$s" 2>&1; then echo "OK: sh -n $s"; else echo "FAIL: sh -n $s"; rc=1; fi
done
if bash -n build.sh 2>&1; then echo "OK: bash -n build.sh"; else echo "FAIL: bash -n build.sh"; rc=1; fi

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

echo "=== 4/4 shellcheck (advisory) ==="
if command -v shellcheck >/dev/null 2>&1; then
  shellcheck -S warning build.sh autopif.sh selftest.sh && echo "OK: shellcheck clean"
else
  echo "SKIP: shellcheck not installed"
fi

echo "=== RESULT: $([ $rc -eq 0 ] && echo PASS || echo FAIL) ==="
exit $rc
