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
SBX_SH=$( { git ls-files '*.sh'; printf '%s\n' tests/action_transaction_test.sh tests/resetprop_backend_test.sh; } 2>/dev/null | sort -u)
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

if SBX_VALIDATE_TMPDIR="$SBX_TMP_ROOT" sh tests/action_transaction_test.sh; then
  echo "OK: action_transaction_test"
else
  echo "FAIL: action_transaction_test"; rc=1
fi
if SBX_VALIDATE_TMPDIR="$SBX_TMP_ROOT" sh tests/resetprop_backend_test.sh; then
  echo "OK: resetprop_backend_test"
else
  echo "FAIL: resetprop_backend_test"; rc=1
fi

SBX_FLAGS='SBX_NATIVE_READ SBX_HIDE SBX_CPU_REVISION SBX_PROC_VERSION SBX_MEMINFO SBX_SYSFS_MAC'
SBX_LIFECYCLE_TEST="$SBX_TMP/operational-lifecycle"
mkdir -p "$SBX_LIFECYCLE_TEST/mod/bin"
cp helpers.sh post-fs-data.sh "$SBX_LIFECYCLE_TEST/mod/"
printf '\nMODEL=old\nSBX_NATIVE_READ=0\nSBX_HIDE=1\nSBX_CPU_REVISION=1\nSBX_PROC_VERSION=1\nSBX_MEMINFO=0\nSBX_SYSFS_MAC=1\nSBX_HIDE=0\n' \
  > "$SBX_LIFECYCLE_TEST/mod/identity.prop"
printf 'MODEL=new\nSBX_NATIVE_READ=1\nSBX_HIDE=0\nSBX_CPU_REVISION=0\nSBX_PROC_VERSION=0\nSBX_MEMINFO=1\nSBX_SYSFS_MAC=0\n' \
  > "$SBX_LIFECYCLE_TEST/candidate.prop"
IDENTITY_FILE="$SBX_LIFECYCLE_TEST/mod/identity.prop"
MODDIR="$SBX_LIFECYCLE_TEST/mod"
# shellcheck source=helpers.sh
. "$SBX_LIFECYCLE_TEST/mod/helpers.sh"
lifecycle_ok=1
identity_preserve_operational_flags \
  "$SBX_LIFECYCLE_TEST/mod/identity.prop" \
  "$SBX_LIFECYCLE_TEST/candidate.prop" || lifecycle_ok=0
for key in $SBX_FLAGS; do
  count=$(grep -c "^${key}=" "$SBX_LIFECYCLE_TEST/candidate.prop")
  [ "$count" = 1 ] || lifecycle_ok=0
done
[ "$(awk -F= '$1=="SBX_NATIVE_READ" {print $2}' "$SBX_LIFECYCLE_TEST/candidate.prop")" = 0 ] || lifecycle_ok=0
[ "$(awk -F= '$1=="SBX_HIDE" {print $2}' "$SBX_LIFECYCLE_TEST/candidate.prop")" = 1 ] || lifecycle_ok=0
[ "$(awk -F= '$1=="SBX_CPU_REVISION" {print $2}' "$SBX_LIFECYCLE_TEST/candidate.prop")" = 1 ] || lifecycle_ok=0
[ "$(awk -F= '$1=="SBX_PROC_VERSION" {print $2}' "$SBX_LIFECYCLE_TEST/candidate.prop")" = 1 ] || lifecycle_ok=0
[ "$(awk -F= '$1=="SBX_MEMINFO" {print $2}' "$SBX_LIFECYCLE_TEST/candidate.prop")" = 0 ] || lifecycle_ok=0
[ "$(awk -F= '$1=="SBX_SYSFS_MAC" {print $2}' "$SBX_LIFECYCLE_TEST/candidate.prop")" = 1 ] || lifecycle_ok=0

cp "$SBX_LIFECYCLE_TEST/candidate.prop" "$SBX_LIFECYCLE_TEST/mod/identity.prop"
printf 'SBX_NATIVE_READ=1\nSBX_HIDE=0\nSBX_CPU_REVISION=0\nSBX_PROC_VERSION=0\nSBX_MEMINFO=1\nSBX_SYSFS_MAC=0\n' \
  > "$SBX_LIFECYCLE_TEST/mod/.operational-flags"
: > "$SBX_LIFECYCLE_TEST/mod/target.txt"
cat > "$SBX_LIFECYCLE_TEST/mod/bin/sandboxid" <<'EOF'
#!/bin/sh
[ "$1" = set-flag ] || exit 1
key=$2
value=$3
tmp="${SBX_TEST_IDENTITY}.tmp.$$"
awk -F= -v k="$key" '$1!=k {print}' "$SBX_TEST_IDENTITY" > "$tmp" || exit 1
printf '%s=%s\n' "$key" "$value" >> "$tmp"
mv "$tmp" "$SBX_TEST_IDENTITY"
EOF
chmod 0755 "$SBX_LIFECYCLE_TEST/mod/bin/sandboxid"
SBX_TEST_IDENTITY="$SBX_LIFECYCLE_TEST/mod/identity.prop" \
  LOGFILE="$SBX_LIFECYCLE_TEST/post-fs-data.log" \
  sh "$SBX_LIFECYCLE_TEST/mod/post-fs-data.sh" >/dev/null 2>&1 || lifecycle_ok=0
[ ! -e "$SBX_LIFECYCLE_TEST/mod/.operational-flags" ] || lifecycle_ok=0
for key in $SBX_FLAGS; do
  count=$(grep -c "^${key}=" "$SBX_LIFECYCLE_TEST/mod/identity.prop")
  [ "$count" = 1 ] || lifecycle_ok=0
done
[ "$(awk -F= '$1=="SBX_NATIVE_READ" {print $2}' "$SBX_LIFECYCLE_TEST/mod/identity.prop")" = 1 ] || lifecycle_ok=0
[ "$(awk -F= '$1=="SBX_HIDE" {print $2}' "$SBX_LIFECYCLE_TEST/mod/identity.prop")" = 0 ] || lifecycle_ok=0
[ "$(awk -F= '$1=="SBX_CPU_REVISION" {print $2}' "$SBX_LIFECYCLE_TEST/mod/identity.prop")" = 0 ] || lifecycle_ok=0
[ "$(awk -F= '$1=="SBX_PROC_VERSION" {print $2}' "$SBX_LIFECYCLE_TEST/mod/identity.prop")" = 0 ] || lifecycle_ok=0
[ "$(awk -F= '$1=="SBX_MEMINFO" {print $2}' "$SBX_LIFECYCLE_TEST/mod/identity.prop")" = 1 ] || lifecycle_ok=0
[ "$(awk -F= '$1=="SBX_SYSFS_MAC" {print $2}' "$SBX_LIFECYCLE_TEST/mod/identity.prop")" = 0 ] || lifecycle_ok=0

# A non-empty target must run seed only. Record every native command to prove
# post-fs-data never falls back to retired device-wide publication commands.
printf 'com.example.target\n' > "$SBX_LIFECYCLE_TEST/mod/target.txt"
cat > "$SBX_LIFECYCLE_TEST/mod/bin/sandboxid" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$SBX_TEST_CALLS"
[ "$1" = seed ]
EOF
chmod 0755 "$SBX_LIFECYCLE_TEST/mod/bin/sandboxid"
: > "$SBX_LIFECYCLE_TEST/calls"
SBX_TEST_CALLS="$SBX_LIFECYCLE_TEST/calls" \
  LOGFILE="$SBX_LIFECYCLE_TEST/post-fs-data-seed.log" \
  sh "$SBX_LIFECYCLE_TEST/mod/post-fs-data.sh" >/dev/null 2>&1 || lifecycle_ok=0
[ "$(wc -l < "$SBX_LIFECYCLE_TEST/calls" | tr -d ' ')" = 1 ] || lifecycle_ok=0
grep -qx 'seed' "$SBX_LIFECYCLE_TEST/calls" || lifecycle_ok=0
if [ "$lifecycle_ok" = 1 ]; then
  echo "OK: operational flags remain unique through replacement and reinstall restoration"
else
  echo "FAIL: operational flag lifecycle preservation"; rc=1
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

SBX_ROTATE_GUARD="$SBX_TMP/rotate-guard"
mkdir -p "$SBX_ROTATE_GUARD/mod"
cp helpers.sh rotate_ids.sh "$SBX_ROTATE_GUARD/mod/"
printf 'version=test\n' > "$SBX_ROTATE_GUARD/mod/module.prop"
printf 'MODEL=unchanged\n' > "$SBX_ROTATE_GUARD/mod/identity.prop"
printf 'sentinel\n' > "$SBX_ROTATE_GUARD/sentinel"
rotate_guard_ok=1
MODDIR="$SBX_ROTATE_GUARD/mod" LOGFILE="$SBX_ROTATE_GUARD/rotate.log" \
  BACKUP_DIR_ROOT="$SBX_ROTATE_GUARD/backups" \
  sh "$SBX_ROTATE_GUARD/mod/rotate_ids.sh" >/dev/null 2>&1 || rotate_guard_ok=0
for retired in ssaid all safe; do
  if MODDIR="$SBX_ROTATE_GUARD/mod" LOGFILE="$SBX_ROTATE_GUARD/rotate.log" \
     BACKUP_DIR_ROOT="$SBX_ROTATE_GUARD/backups" \
     sh "$SBX_ROTATE_GUARD/mod/rotate_ids.sh" "$retired" >/dev/null 2>&1; then
    rotate_guard_ok=0
  fi
done
[ "$(cat "$SBX_ROTATE_GUARD/mod/identity.prop")" = 'MODEL=unchanged' ] || rotate_guard_ok=0
[ "$(cat "$SBX_ROTATE_GUARD/sentinel")" = 'sentinel' ] || rotate_guard_ok=0
if [ "$rotate_guard_ok" = 1 ]; then
  echo "OK: no-arg rotation is help-only and retired aggregate/SSAID commands fail closed"
else
  echo "FAIL: retired rotation command mutated state or returned success"; rc=1
fi

PROHIBITED_RE='settings_ssaid\.xml|Regenerasi SSAID|Rotasi semua|rotate_ids\.sh all|pm clear'
if grep -R -n -E "$PROHIBITED_RE" action.sh post-fs-data.sh service.sh rotate_ids.sh webroot customize.sh module.prop devices.tsv >/dev/null 2>&1; then
  echo "FAIL: runtime/UI source still contains destructive or unsupported lifecycle flow"; rc=1
else
  echo "OK: runtime/UI source omits SSAID reset, aggregate rotation, and app-data clearing"
fi
if grep -R -n -E 'resetprop.*(--delete|-d)' jni action.sh post-fs-data.sh service.sh >/dev/null 2>&1; then
  echo "FAIL: normal lifecycle source still deletes global properties"; rc=1
else
  echo "OK: normal lifecycle source contains no global property deletion"
fi
if grep -n -E 'apply-props|apply-boot' action.sh post-fs-data.sh service.sh >/dev/null 2>&1; then
  echo "FAIL: boot/action scripts still invoke retired device-wide commands"; rc=1
else
  echo "OK: boot/action scripts never invoke retired device-wide commands"
fi
if ! grep -q '"$BIN" import "$DEVICE_ID"' action.sh \
   || grep -n -E 'cp -f .*DEVICE_ID.*IDENTITY|mv -f .*IDENTITY' action.sh >/dev/null 2>&1; then
  echo "FAIL: Action bypasses validated native identity import"; rc=1
else
  echo "OK: Action uses validated atomic native identity import"
fi

if ! grep -q 'SBX_HIDE=0' autopif.sh \
   || ! grep -q -E 'should_hide_prop\([^,]+, false\)' tests/native_read_test.cpp \
   || ! grep -q -E 'should_hide_prop\([^,]+, true\)' tests/native_read_test.cpp; then
  echo "FAIL: explicit property-hide opt-in coverage missing"; rc=1
else
  echo "OK: broad property hiding defaults off and has host regression coverage"
fi

if grep -n -E 'session-[0-9]|\.claude-tmp' build.sh >/dev/null 2>&1; then
  echo "FAIL: build script references local session artifacts"; rc=1
else
  echo "OK: build script does not package local session artifacts"
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
