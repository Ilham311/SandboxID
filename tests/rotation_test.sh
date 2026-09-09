#!/usr/bin/env bash
# Assertions evaluate variable names from test expressions.
# shellcheck disable=SC2034
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP_ROOT="${TMPDIR:-$ROOT/.claude-tmp}"
WORK=$(mktemp -d "$TMP_ROOT/sbx-rotation-test.XXXXXX") || exit 1
trap 'if [ "${SBX_KEEP_TEST_TMP:-0}" = 1 ]; then printf "kept: %s\\n" "$WORK"; else rm -rf "$WORK"; fi' EXIT

checks=0
failures=0
check() {
    checks=$((checks + 1))
    if ! eval "$1"; then
        failures=$((failures + 1))
        printf 'FAIL: %s\n' "$2"
    fi
}

mkdir -p "$WORK/mod/bin" "$WORK/tools" "$WORK/mod/debug" "$WORK/backups"
cp "$ROOT/helpers.sh" "$ROOT/rotate_ids.sh" "$WORK/mod/"
printf 'version=test\n' > "$WORK/mod/module.prop"
printf '%s\n' \
    'GOOGLE_AID=12345678-1234-4abc-8def-1234567890ab' \
    'WIFI_MAC=02:11:22:33:44:55' \
    'BLUETOOTH_ADDR=02:66:77:88:99:aa' \
    'BLUETOOTH_NAME=Sandbox Test' \
    'BOOT_COUNT=17' \
    'APPLOG_EPOCH=1700000000000' > "$WORK/mod/identity.prop"
printf 'com.example.app\n' > "$WORK/mod/target.txt"
mkdir -p "$WORK/fake-data/com.google.android.gms/shared_prefs"
chmod 0777 "$WORK/fake-data/com.google.android.gms" \
    "$WORK/fake-data/com.google.android.gms/shared_prefs"
printf '%s\n' '#!/bin/sh' \
    'printf "%s\n" "$*" >> "$SBX_NATIVE_CALLS"' \
    'if [ "$1" = targets ] && [ "$2" = --packages ]; then' \
    '  printf "com.example.app\n"' \
    'fi' \
    'exit "${SBX_NATIVE_RC:-0}"' > "$WORK/mod/bin/sandboxid"
printf '%s\n' '#!/bin/sh' \
    'case "$1 $2 $3" in' \
    '  "link show wlan0") exit "${SBX_IP_SHOW_RC:-0}" ;;' \
    'esac' \
    'printf "%s\n" "$*" >> "$SBX_IP_CALLS"' > "$WORK/tools/ip"
printf '#!/bin/sh\nexit 0\n' > "$WORK/tools/settings"
printf '#!/bin/sh\nexit 0\n' > "$WORK/tools/am"
printf '#!/bin/sh\nexit 0\n' > "$WORK/tools/setprop"
printf '#!/bin/sh\nexit 0\n' > "$WORK/tools/getenforce"
printf '#!/bin/sh\nexit "${SBX_CHOWN_RC:-0}"\n' > "$WORK/tools/chown"
printf '#!/bin/sh\nexit 0\n' > "$WORK/tools/chmod"
printf '#!/bin/sh\nexit 0\n' > "$WORK/tools/setenforce"
printf '#!/bin/sh\nexit 0\n' > "$WORK/tools/chcon"
chmod 0755 "$WORK/mod/bin/sandboxid" "$WORK/tools/"*

export SBX_NATIVE_CALLS="$WORK/native.calls" SBX_IP_CALLS="$WORK/ip.calls"

: > "$WORK/native.calls"
PATH="$WORK/tools:$PATH" MODDIR="$WORK/mod" LOGFILE="$WORK/rotate.log" \
    BACKUP_DIR_ROOT="$WORK/backups" sh "$WORK/mod/rotate_ids.sh" \
    boot-count 23 > "$WORK/standalone.log" 2>&1
standalone_rc=$?
check '[ "$standalone_rc" -eq 0 ]' 'standalone boot-count succeeds with native writer'
check 'grep -q "^set-local BOOT_COUNT 23$" "$WORK/native.calls"' 'standalone mutation publishes canonical value first'

: > "$WORK/native.calls"
PATH="$WORK/tools:$PATH" MODDIR="$WORK/mod" LOGFILE="$WORK/rotate.log" \
    BACKUP_DIR_ROOT="$WORK/backups" sh "$WORK/mod/rotate_ids.sh" \
    boot-count > "$WORK/standalone-fresh.log" 2>&1
standalone_fresh_rc=$?
check '[ "$standalone_fresh_rc" -eq 0 ]' 'no-argument standalone boot-count succeeds'
check 'grep -q "^set-local BOOT_COUNT 18$" "$WORK/native.calls"' 'no-argument standalone rotation generates a value distinct from canonical snapshot'

: > "$WORK/native.calls"
PATH="$WORK/tools:$PATH" MODDIR="$WORK/mod" LOGFILE="$WORK/rotate.log" \
    BACKUP_DIR_ROOT="$WORK/backups" sh "$WORK/mod/rotate_ids.sh" \
    status > "$WORK/status.log" 2>&1
status_rc=$?
check '[ "$status_rc" -eq 0 ]' 'status succeeds with normalized target lookup'
check 'grep -q "^targets --packages$" "$WORK/native.calls"' 'status obtains base packages from native target parser'

: > "$WORK/native.calls"
SBX_NATIVE_RC=64 PATH="$WORK/tools:$PATH" MODDIR="$WORK/mod" \
    LOGFILE="$WORK/rotate.log" BACKUP_DIR_ROOT="$WORK/backups" \
    sh "$WORK/mod/rotate_ids.sh" boot-count 24 > "$WORK/native-fail.log" 2>&1
native_fail_rc=$?
check '[ "$native_fail_rc" -ne 0 ]' 'standalone external mutation stops when native set-local rejects'

mkdir -p "$WORK/mod/.action.lock" "$WORK/mod/.mutation.lock"
start=$(awk '{print $22; exit}' "/proc/$$/stat")
token=44444444444444444444444444444444
run=0123456789abcdef0123456789abcdef
printf 'version=1\nkind=action\npid=%s\nproc_start=%s\nrun=%s\ntoken=%s\n' \
    "$$" "$start" "$run" "$token" > "$WORK/mod/.action.lock/owner"
cp "$WORK/mod/.action.lock/owner" "$WORK/mod/.mutation.lock/owner"
: > "$WORK/native.calls"
rm -f "$WORK/tools/ip"
PATH="$WORK/tools:$PATH" MODDIR="$WORK/mod" LOGFILE="$WORK/action.log" \
    BACKUP_DIR_ROOT="$WORK/backups" SBX_GMS_DIR="$WORK/fake-data/com.google.android.gms" \
    SBX_ACTION_RUN_ID="$run" SBX_MUTATION_OWNER_PID="$$" \
    SBX_MUTATION_OWNER_START="$start" SBX_MUTATION_OWNER_TOKEN="$token" \
    sh "$WORK/mod/rotate_ids.sh" all --from-identity > "$WORK/action.out" 2>&1
action_rc=$?
REPORT="$WORK/mod/debug/rotation.0123456789abcdef0123456789abcdef"
check '[ "$action_rc" -eq 3 ]' 'required unsupported component returns partial status'
check '[ -s "$REPORT" ]' 'Action rotation report is published'
check 'grep -q "^run=0123456789abcdef0123456789abcdef$" "$REPORT"' 'report is bound to Action run'
check 'grep -q "^wlan_mac=unsupported:3$" "$REPORT"' 'report records unsupported Wi-Fi component'
check 'grep -q "^UNSUPPORTED=1$" "$REPORT"' 'unsupported count matches component status'
check 'grep -q "^gaid=ok:0$" "$REPORT" && grep -q "^boot_count=ok:0$" "$REPORT"' 'report records successful components'
check '! grep -q "^set-local " "$WORK/native.calls"' 'Action snapshot mode does not rewrite canonical local IDs'

rm -rf "$WORK/mod/.action.lock" "$WORK/mod/.mutation.lock"
mkdir -p "$WORK/fake-data/com.google.android.gms/shared_prefs"
printf '%s\n' '<map><string name="adid_key">old-value</string></map>' \
    > "$WORK/fake-data/com.google.android.gms/shared_prefs/adid_settings.xml"
SBX_CHOWN_RC=1 PATH="$WORK/tools:$PATH" MODDIR="$WORK/mod" \
    LOGFILE="$WORK/rotate.log" BACKUP_DIR_ROOT="$WORK/backups" \
    SBX_GMS_DIR="$WORK/fake-data/com.google.android.gms" \
    sh "$WORK/mod/rotate_ids.sh" gaid 87654321-4321-4abc-8def-ba0987654321 \
    > "$WORK/gaid-restore.log" 2>&1
gaid_restore_rc=$?
check '[ "$gaid_restore_rc" -ne 0 ]' 'GAID publication failure is reported'
check 'grep -q "old-value" "$WORK/fake-data/com.google.android.gms/shared_prefs/adid_settings.xml"' 'GAID publication failure restores the backed-up primary XML'

printf '\n%d checks, %d failures\n' "$checks" "$failures"
exit "$failures"
