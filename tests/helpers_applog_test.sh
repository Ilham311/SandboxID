#!/usr/bin/env bash
# Assertions evaluate variable names from test expressions.
# shellcheck disable=SC2034
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP_ROOT="${TMPDIR:-$ROOT/.claude-tmp}"
mkdir -p "$TMP_ROOT"
WORK=$(mktemp -d "$TMP_ROOT/sbx-applog-test.XXXXXX") || exit 1
trap 'if [ "${SBX_KEEP_TEST_TMP:-0}" = 1 ]; then printf "kept: %s\n" "$WORK"; else rm -rf "$WORK"; fi' EXIT

checks=0
failures=0
check() {
    checks=$((checks + 1))
    if ! eval "$1"; then
        failures=$((failures + 1))
        printf 'FAIL: %s\n' "$2"
    fi
}

mkdir -p "$WORK/mod/bin" "$WORK/tools" "$WORK/data" "$WORK/user0" "$WORK/backups"
python3 - "$ROOT/helpers.sh" "$WORK/mod/helpers.sh" "$WORK/data" "$WORK/user0" <<'PY'
import pathlib, shlex, sys
src, dst, data, user0 = sys.argv[1:]
text = pathlib.Path(src).read_text()
old = 'for _base in /data/data /data/user/0; do'
new = f'for _base in {shlex.quote(data)} {shlex.quote(user0)}; do'
assert text.count(old) == 4
pathlib.Path(dst).write_text(text.replace(old, new))
PY

REAL_RM=$(command -v rm)
REAL_LS=$(command -v ls)
REAL_TAR=$(command -v tar)
export REAL_RM REAL_LS REAL_TAR

cat > "$WORK/mod/bin/sandboxid" <<'SH'
#!/bin/sh
[ "$1" = applog-ids ] || exit 64
printf '%s\n' 'DID=1111111111111111111' 'IID=2222222222222222222' \
  'SSID=3333333333333333333' 'OPENUDID=0123456789abcdef' \
  'CLIENTUDID=fedcba9876543210' 'CDID=12345678-1234-4abc-8def-1234567890ab'
SH
cat > "$WORK/tools/am" <<'SH'
#!/bin/sh
printf 'am %s\n' "$*" >> "$SBX_CALLS"
exit "${SBX_AM_RC:-0}"
SH
cat > "$WORK/tools/killall" <<'SH'
#!/bin/sh
exit 0
SH
cat > "$WORK/tools/getenforce" <<'SH'
#!/bin/sh
printf 'Enforcing\n'
SH
cat > "$WORK/tools/setenforce" <<'SH'
#!/bin/sh
printf 'setenforce %s\n' "$*" >> "$SBX_CALLS"
[ "$1" = 1 ] && exit "${SBX_SETENFORCE_RESTORE_RC:-0}"
exit 0
SH
cat > "$WORK/tools/chown" <<'SH'
#!/bin/sh
exit "${SBX_CHOWN_RC:-0}"
SH
cat > "$WORK/tools/chmod" <<'SH'
#!/bin/sh
exit "${SBX_CHMOD_RC:-0}"
SH
cat > "$WORK/tools/chcon" <<'SH'
#!/bin/sh
exit "${SBX_CHCON_RC:-0}"
SH
cat > "$WORK/tools/ls" <<'SH'
#!/bin/sh
if [ "$1" = -Zd ]; then
    printf '%s %s\n' "${SBX_CONTEXT:-u:object_r:app_data_file:s0}" "$2"
    exit 0
fi
exec "$REAL_LS" "$@"
SH
cat > "$WORK/tools/tar" <<'SH'
#!/bin/sh
if [ "${SBX_TAR_RC:-0}" -ne 0 ]; then exit "$SBX_TAR_RC"; fi
exec "$REAL_TAR" "$@"
SH
cat > "$WORK/tools/rm" <<'SH'
#!/bin/sh
for arg in "$@"; do
    [ -n "${SBX_RM_BLOCK:-}" ] && [ "$arg" = "$SBX_RM_BLOCK" ] && exit 1
done
exec "$REAL_RM" "$@"
SH
chmod 0755 "$WORK/mod/bin/sandboxid" "$WORK/tools/"*

make_pkg() {
    pkg="$1"
    root="$WORK/data/$pkg"
    mkdir -p "$root/shared_prefs" "$root/files/bd_setting" "$root/no_backup"
    printf 'pref\n' > "$root/shared_prefs/applog.xml"
    printf 'did\n' > "$root/files/bd_setting/device_id"
    mkdir -p "$root/files/applog_v2"
    printf 'nested\n' > "$root/files/applog_v2/cache"
    printf 'nb\n' > "$root/no_backup/.cdid"
}

run_helper() {
    body="$1"
    PATH="$WORK/tools:$PATH" MODDIR="$WORK/mod" IDENTITY_FILE="$WORK/mod/identity.prop" \
      BACKUP_DIR_ROOT="$WORK/backups" LOGFILE="$WORK/helper.log" TMPDIR="$WORK" \
      SBX_CALLS="$WORK/calls" sh -c ". '$WORK/mod/helpers.sh'; $body"
}

make_pkg com.example.good
: > "$WORK/calls"
run_helper 'applog_wipe com.example.good' > "$WORK/wipe-good.log" 2>&1
wipe_good_rc=$?
archive=$(find "$WORK/backups" -type f -name 'applog_com_example_good_*.tar' -print -quit)
check '[ "$wipe_good_rc" -eq 0 ]' 'AppLog wipe succeeds after complete backup'
check '[ -n "$archive" ] && [ -s "$archive" ]' 'wipe publishes a nonempty backup archive'
check '"$REAL_TAR" -tf "$archive" | grep -q "shared_prefs/applog.xml"' 'backup includes selected shared preference'
check '"$REAL_TAR" -tf "$archive" | grep -q "files/bd_setting/device_id"' 'backup includes selected files path'
check '"$REAL_TAR" -tf "$archive" | grep -q "files/applog_v2"' 'backup includes selected directory tree'
check '"$REAL_TAR" -tf "$archive" | grep -q "no_backup/.cdid"' 'backup includes selected no_backup path'
check '[ ! -e "$WORK/data/com.example.good/shared_prefs/applog.xml" ] && [ ! -e "$WORK/data/com.example.good/files/applog_v2" ]' 'all selected cache paths are deleted after backup'
check 'grep -q "setenforce 0" "$WORK/calls" && grep -q "setenforce 1" "$WORK/calls"' 'SELinux state is unwound after wipe'

make_pkg com.example.stopfail
before=$(sha256sum "$WORK/data/com.example.stopfail/shared_prefs/applog.xml" | cut -d' ' -f1)
SBX_AM_RC=1 run_helper 'applog_wipe com.example.stopfail' > "$WORK/stop-fail.log" 2>&1
stop_rc=$?
after=$(sha256sum "$WORK/data/com.example.stopfail/shared_prefs/applog.xml" | cut -d' ' -f1)
check '[ "$stop_rc" -ne 0 ]' 'force-stop failure rejects wipe'
check '[ "$before" = "$after" ]' 'force-stop failure leaves cache untouched'

mkdir -p "$WORK/data/escape"
printf 'sentinel\n' > "$WORK/data/escape/applog.xml"
run_helper 'applog_wipe ../escape' > "$WORK/invalid-package.log" 2>&1
invalid_package_rc=$?
check '[ "$invalid_package_rc" -eq 64 ]' 'malformed explicit package is rejected before path construction'
check '[ -e "$WORK/data/escape/applog.xml" ]' 'malformed explicit package cannot reach another data path'

make_pkg com.example.restorefail
SBX_SETENFORCE_RESTORE_RC=1 run_helper 'applog_wipe com.example.restorefail' > "$WORK/restore-fail.log" 2>&1
restore_rc=$?
check '[ "$restore_rc" -ne 0 ]' 'SELinux enforcing restore failure is propagated'
check 'grep -q "SELinux enforcing tidak dapat dipulihkan" "$WORK/restore-fail.log"' 'SELinux restore failure is reported'

make_pkg com.example.backupfail
SBX_TAR_RC=1 run_helper 'applog_wipe com.example.backupfail' > "$WORK/backup-fail.log" 2>&1
backup_rc=$?
check '[ "$backup_rc" -ne 0 ]' 'backup failure rejects wipe'
check '[ -e "$WORK/data/com.example.backupfail/shared_prefs/applog.xml" ] && [ -e "$WORK/data/com.example.backupfail/files/applog_v2/cache" ]' 'backup failure leaves all selected cache untouched'

make_pkg com.example.deletefail
blocked="$WORK/data/com.example.deletefail/files/bd_setting/device_id"
SBX_RM_BLOCK="$blocked" run_helper 'applog_wipe com.example.deletefail' > "$WORK/delete-fail.log" 2>&1
delete_rc=$?
check '[ "$delete_rc" -ne 0 ]' 'verified deletion failure returns partial failure'
check '[ -e "$blocked" ]' 'failed deletion remains detectable'

rm -rf "$WORK/data/com.example.seed"
mkdir -p "$WORK/data/com.example.seed"
run_helper 'applog_seed com.example.seed' > "$WORK/seed-good.log" 2>&1
seed_good_rc=$?
check '[ "$seed_good_rc" -eq 0 ]' 'AppLog seed succeeds with verifiable ownership and context'
check 'grep -q "1111111111111111111" "$WORK/data/com.example.seed/shared_prefs/applog.xml"' 'seed uses native deterministic DID'
check '[ -s "$WORK/data/com.example.seed/files/bd_setting/openudid" ] && [ -s "$WORK/data/com.example.seed/files/.cdid" ]' 'seed publishes all selected files'

rm -rf "$WORK/data/com.example.contextfail"
mkdir -p "$WORK/data/com.example.contextfail"
SBX_CONTEXT='?' run_helper 'applog_seed com.example.contextfail' > "$WORK/context-fail.log" 2>&1
context_rc=$?
check '[ "$context_rc" -ne 0 ]' 'unverifiable SELinux context rejects seed'
check '[ ! -e "$WORK/data/com.example.contextfail/shared_prefs/applog.xml" ]' 'context failure publishes no seed files'

rm -rf "$WORK/data/com.example.ownerfail"
mkdir -p "$WORK/data/com.example.ownerfail"
SBX_CHOWN_RC=1 run_helper 'applog_seed com.example.ownerfail' > "$WORK/owner-fail.log" 2>&1
owner_rc=$?
check '[ "$owner_rc" -ne 0 ]' 'ownership failure rejects seed'
check '[ ! -e "$WORK/data/com.example.ownerfail/shared_prefs/applog.xml" ]' 'ownership failure leaves no root-owned seed file'

sh -n "$ROOT/helpers.sh" "$WORK/mod/helpers.sh" || failures=$((failures + 1))
printf '\n%d checks, %d failures\n' "$checks" "$failures"
exit "$failures"
