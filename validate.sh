#!/usr/bin/env bash
set -u
cd "$(dirname "$0")" || exit 1
rc=0

TMP_ROOT="${SBX_VALIDATE_TMPDIR:-${TMPDIR:-$PWD/.claude-tmp}}"
if ! mkdir -p "$TMP_ROOT" 2>/dev/null || [ ! -w "$TMP_ROOT" ]; then
  echo "FAIL: validation temp directory is not writable: $TMP_ROOT"
  exit 1
fi
TMP=$(mktemp -d "$TMP_ROOT/sbx-validate.XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT

pass() { echo "OK: $*"; }
fail() { echo "FAIL: $*"; rc=1; }
run_test() {
  _name=$1
  shift
  if "$@"; then pass "$_name"; else fail "$_name"; fi
}

echo "=== 1/8 native syntax ==="
for file in jni/companion.cpp jni/sandboxid.cpp; do
  run_test "clang++ debug $file" clang++ -std=c++20 -fsyntax-only -DSBX_DEBUG=1 -Wall -Wextra -Ijni "$file"
  run_test "clang++ release $file" clang++ -std=c++20 -fsyntax-only -Wall -Wextra -Ijni "$file"
done
if [ -f jni/zygisk.hpp ]; then
  run_test "clang++ debug jni/main.cpp" clang++ -std=c++20 -fsyntax-only -DSBX_DEBUG=1 -Wall -Wextra -Ijni jni/main.cpp
  run_test "clang++ release jni/main.cpp" clang++ -std=c++20 -fsyntax-only -Wall -Wextra -Ijni jni/main.cpp
else
  echo "BLOCKED: jni/main.cpp syntax requires missing generated jni/zygisk.hpp"
fi

echo "=== 2/8 shell and JavaScript syntax ==="
SH_FILES=$(git ls-files '*.sh' 2>/dev/null)
[ -n "$SH_FILES" ] || SH_FILES=$(find . -name '*.sh' -not -path './.git/*' -not -path './build/*' -not -path './dist/*' | sed 's|^./||')
for file in $SH_FILES; do
  if grep -q '^#!/.*bash' "$file"; then
    run_test "bash -n $file" bash -n "$file"
  else
    run_test "sh -n $file" sh -n "$file"
  fi
done
if command -v node >/dev/null 2>&1; then
  run_test "node syntax theme-init" node --check webroot/theme-init.js
  run_test "node syntax app" node --check webroot/app.js
else
  echo "SKIP: node not installed"
fi

echo "=== 3/8 C++ host suites ==="
for suite in native_read persona target; do
  if clang++ -std=c++20 -Wall -Wextra -Ijni -o "$TMP/sbx_${suite}_test" "tests/${suite}_test.cpp" &&
     "$TMP/sbx_${suite}_test"; then
    pass "${suite}_test"
  else
    fail "${suite}_test"
  fi
done
if command -v python3 >/dev/null 2>&1; then
  run_test "probe expectations" python3 tests/probe_expectations_test.py
else
  fail "python3 required for probe expectations"
fi

echo "=== 4/8 safe mocked lifecycle suites ==="
for suite in action autopif rotation helpers_applog customize package_manifest; do
  if [ "$suite" = package_manifest ]; then
    run_test "$suite" bash "tests/${suite}_test.sh" --source
  else
    run_test "$suite" bash "tests/${suite}_test.sh"
  fi
done
if [ -f tests/webui_test.js ]; then
  if command -v node >/dev/null 2>&1; then
    run_test "webui behavior" node tests/webui_test.js
  else
    fail "node required for tests/webui_test.js"
  fi
else
  fail "tests/webui_test.js is missing"
fi

echo "=== 5/8 acquisition contract ==="
if grep -q 'ADAPTER="${AUTOPIF_ADAPTER:-pixel-ota-v1}"' autopif.sh &&
   grep -q 'meta\["adapter"\] != "pixel-ota-v1"' jni/sbx_transaction.hpp; then
  pass "pixel-ota-v1 adapter is synchronized"
else
  fail "acquisition adapter contract is inconsistent"
fi
if grep -q -- '--location' autopif.sh; then
  fail "autopif follows redirects"
else
  pass "autopif refuses implicit redirects"
fi

echo "=== 6/8 package blockers and source policy ==="
if [ -f module.prop ]; then
  pass "module.prop present"
else
  echo "BLOCKED: module.prop is absent; Android package build was not run"
fi
if [ -f jni/zygisk.hpp ]; then
  pass "jni/zygisk.hpp present"
else
  echo "BLOCKED: jni/zygisk.hpp is absent; Zygisk/JNI syntax was not checked"
fi
if grep -q 'devices.tsv\|AUTOPIF_REFRESH' build.sh; then
  fail "build.sh retains obsolete acquisition inputs"
else
  pass "build.sh is deterministic and devices.tsv-free"
fi

echo "=== 7/8 repository whitespace ==="
if git diff --check; then pass "git diff --check"; else fail "git diff --check"; fi

echo "=== 8/8 shellcheck ==="
if command -v shellcheck >/dev/null 2>&1; then
  if shellcheck -S warning $SH_FILES; then pass "shellcheck"; else fail "shellcheck"; fi
else
  echo "SKIP: shellcheck not installed (CI must still enforce it)"
fi

echo "=== RESULT: $([ "$rc" -eq 0 ] && echo PASS || echo FAIL) ==="
exit "$rc"
