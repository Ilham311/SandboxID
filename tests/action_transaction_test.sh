#!/bin/sh
set -u

ROOT=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
TMP_ROOT=${SBX_VALIDATE_TMPDIR:-${TMPDIR:-$ROOT/.claude-tmp}}
mkdir -p "$TMP_ROOT" || exit 1
TMP=$(mktemp -d "$TMP_ROOT/sbx-action-test.XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT INT TERM

MOD="$TMP/mod"
TRAP_BIN="$TMP/trap-bin"
CALLS="$TMP/calls"
FORBIDDEN="$TMP/forbidden"
IDENTITY="$MOD/identity.prop"
mkdir -p "$MOD/bin" "$MOD/debug" "$TRAP_BIN"
cp "$ROOT/action.sh" "$ROOT/helpers.sh" "$MOD/"
printf 'fixture\n' > "$MOD/devices.tsv"
: > "$MOD/target.txt"
printf 'target-login-and-permissions\n' > "$TMP/target.sentinel"
printf 'non-target-state\n' > "$TMP/non-target.sentinel"

cat > "$MOD/autopif.sh" <<'EOF'
#!/bin/sh
case "${SBX_TEST_MODE:-}" in
    oem-ok|oem-fail)
        cat > "$MODDIR/device.identity" <<'IDENTITY'
BRAND=Fixture
MARKETNAME=Fixture OEM
MODEL=OEM
DEVICE=fixture
RELEASE=15
SDK_INT=35
FINGERPRINT=fixture/oem/device:15/ID/inc:user/release-keys
BOOT_COUNT=7
UPTIME_HUMAN=1d
SERIAL=OEMSERIAL
ANDROID_ID=0123456789abcdef
IDENTITY
        ;;
    no-artifact) : ;;
    *) exit 64 ;;
esac
EOF

cat > "$MOD/bin/sandboxid" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "${SBX_TEST_CALLS:?}"
case "${1:-}" in
    unlock|lock) exit 0 ;;
    import)
        [ "${SBX_TEST_MODE:-}" = oem-fail ] && exit "${SBX_IMPORT_RC:-37}"
        cp "$2" "${SBX_TEST_IDENTITY:?}"
        ;;
    freshen)
        printf 'MODEL=Pixel fallback\n' > "${SBX_TEST_IDENTITY:?}"
        ;;
    *) exit 65 ;;
esac
EOF

cat > "$TRAP_BIN/pm" <<'EOF'
#!/bin/sh
printf '%s %s\n' "${0##*/}" "$*" >> "${SBX_FORBIDDEN_LOG:?}"
exit 99
EOF
cp "$TRAP_BIN/pm" "$TRAP_BIN/am"
cat > "$MOD/rotate_ids.sh" <<'EOF'
#!/bin/sh
printf 'rotate_ids.sh %s\n' "$*" >> "${SBX_FORBIDDEN_LOG:?}"
exit 99
EOF
chmod 0755 "$MOD/autopif.sh" "$MOD/bin/sandboxid" "$MOD/rotate_ids.sh" \
    "$TRAP_BIN/pm" "$TRAP_BIN/am"

rc=0
check_eq() {
    label=$1 expected=$2 actual=$3
    if [ "$expected" != "$actual" ]; then
        printf 'FAIL: %s (expected %s, got %s)\n' "$label" "$expected" "$actual" >&2
        rc=1
    fi
}
check_true() {
    label=$1
    shift
    if ! "$@"; then
        printf 'FAIL: %s\n' "$label" >&2
        rc=1
    fi
}
count_call() {
    grep -c "^$1" "$CALLS" 2>/dev/null || true
}
reset_case() {
    printf 'MODEL=Original\nSESSION=preserve-me\n' > "$IDENTITY"
    printf 'stale artifact\n' > "$MOD/device.identity"
    : > "$CALLS"
    : > "$FORBIDDEN"
}
run_action() {
    mode=$1
    action_log="$TMP/$mode.log"
    rm -f "$action_log"
    PATH="$TRAP_BIN:$PATH" SBX_TEST_MODE="$mode" SBX_IMPORT_RC=37 \
        SBX_TEST_CALLS="$CALLS" SBX_FORBIDDEN_LOG="$FORBIDDEN" \
        SBX_TEST_IDENTITY="$IDENTITY" LOGFILE="$action_log" \
        sh "$MOD/action.sh" > "$TMP/$mode.stdout" 2>&1
    action_rc=$?
}
assert_untouched_state() {
    check_eq "target data sentinel" target-login-and-permissions \
        "$(tr -d '\n' < "$TMP/target.sentinel")"
    check_eq "non-target sentinel" non-target-state \
        "$(tr -d '\n' < "$TMP/non-target.sentinel")"
    check_eq "forbidden pm/am/rotator calls" 0 "$(wc -c < "$FORBIDDEN" | tr -d ' ')"
    check_true "caller-provided LOGFILE used" test -s "$action_log"
}

reset_case
run_action oem-ok
check_eq "OEM success status" 0 "$action_rc"
check_eq "OEM artifact imported once" 1 "$(count_call 'import ')"
check_eq "OEM success does not freshen" 0 "$(count_call 'freshen$')"
check_true "OEM persona activated" grep -q '^MODEL=OEM$' "$IDENTITY"
assert_untouched_state

reset_case
run_action oem-fail
check_eq "failed import exact status" 37 "$action_rc"
check_eq "failed OEM artifact imported once" 1 "$(count_call 'import ')"
check_eq "failed OEM import never freshens" 0 "$(count_call 'freshen$')"
check_true "failed import preserves identity" grep -q '^SESSION=preserve-me$' "$IDENTITY"
check_true "failed import reports native status" grep -q 'rc=37' "$action_log"
assert_untouched_state

reset_case
run_action no-artifact
check_eq "no-artifact fallback status" 0 "$action_rc"
check_eq "no-artifact path does not import" 0 "$(count_call 'import ')"
check_eq "no-artifact path freshens once" 1 "$(count_call 'freshen$')"
check_true "fallback persona activated" grep -q '^MODEL=Pixel fallback$' "$IDENTITY"
assert_untouched_state

if [ "$rc" -eq 0 ]; then
    echo "OK: Action transaction is single-persona, exact-status, and non-destructive"
fi
exit "$rc"
