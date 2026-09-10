#!/usr/bin/env bash
# Assertions evaluate variable names from test expressions.
# shellcheck disable=SC2034
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP_ROOT="${TMPDIR:-$ROOT/.claude-tmp}"
mkdir -p "$TMP_ROOT"
WORK=$(mktemp -d "$TMP_ROOT/sbx-action-test.XXXXXX") || exit 1
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

RUN_ID=0123456789abcdef0123456789abcdef
CASE=""
CASE_RC=0

reset_env() {
    export SBX_TARGETS='com.example.present\ncom.example.absent\n'
    export SBX_PACKAGES='package:com.example.present\n'
    export SBX_TARGETS_RC=0 SBX_PREPARE_RC=0 SBX_COMMIT_RC=0
    export SBX_APPLY_PROPS_RC=0 SBX_APPLY_BOOT_RC=0
    export SBX_REAPPLY_PROPS_RC=0 SBX_REAPPLY_BOOT_RC=0
    export SBX_RESTORE_RC=0 SBX_VERIFY_RUN_RC=0 SBX_VERIFY_RESTORE_RC=0
    export SBX_COMMIT_DIGEST=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
    export SBX_ROTATE_RC=0 SBX_ROTATE_FAILURES=0 SBX_ROTATE_UNSUPPORTED=0
    export SBX_ROTATE_REBOOT=0 SBX_PM_CLEAR_FAIL='' SBX_PM_USER_QUERY_RC=0 SBX_PM_QUERY_RC=0 SBX_AM_FAIL=''
    export SBX_PM_FILTER_USER_RC=0 SBX_PM_FILTER_RC=0 SBX_PM_FILTER_FAIL=''
    export SBX_PM_USER_QUERY_SEQUENCE='' SBX_PM_QUERY_SEQUENCE=''
    export SBX_PM_FILTER_USER_SEQUENCE='' SBX_PM_FILTER_SEQUENCE=''
    export SBX_PM_FAIL_STDOUT='' SBX_PM_FAIL_STDERR=''
    export SBX_APPLOG_WIPE_FAIL='' SBX_APPLOG_SEED_FAIL=''
}

setup_case() {
    CASE="$WORK/$1"
    mkdir -p "$CASE/mod/bin" "$CASE/mod/debug" "$CASE/tools"
    cp "$ROOT/action.sh" "$CASE/mod/action.sh"
    printf 'fresh\n' > "$CASE/mod/identity.mode"
    printf '# mock targets\n' > "$CASE/mod/target.txt"
    printf '#!/bin/sh\nexit 0\n' > "$CASE/mod/bin/resetprop-rs"
    make_helpers
    make_native
    make_rotate
    make_tools
    chmod 0755 "$CASE/mod/action.sh" "$CASE/mod/bin/"* "$CASE/tools/"*
    reset_env
}

make_helpers() {
    cat > "$CASE/mod/helpers.sh" <<'SH'
#!/bin/sh
sbx_bin() { printf '%s\n' "$MODDIR/bin/sandboxid"; }
proc_start_ticks() { awk '{print $22; exit}' "/proc/$1/stat"; }
mutation_lock_acquire() {
    kind="${1:-standalone}"; run="${2:-}"
    if mkdir "$MODDIR/.mutation.lock" 2>/dev/null; then
        :
    elif [ -r "$MODDIR/.mutation.lock/owner" ]; then
        pid="$(awk -F= '$1=="pid"{print $2;exit}' "$MODDIR/.mutation.lock/owner")"
        start="$(awk -F= '$1=="proc_start"{print $2;exit}' "$MODDIR/.mutation.lock/owner")"
        current="$(proc_start_ticks "$pid" 2>/dev/null)"
        [ -n "$current" ] && [ "$current" = "$start" ] && return 75
        stale="$MODDIR/.mutation.lock.stale.$$"
        mv "$MODDIR/.mutation.lock" "$stale" 2>/dev/null || return 75
        rm -rf "$stale" || return 1
        mkdir "$MODDIR/.mutation.lock" || return 75
    else
        return 75
    fi
    start="$(proc_start_ticks "$$")"
    token=11111111111111111111111111111111
    {
        printf 'version=1\nkind=%s\npid=%s\nproc_start=%s\n' "$kind" "$$" "$start"
        [ -n "$run" ] && printf 'run=%s\n' "$run"
        printf 'token=%s\n' "$token"
    } > "$MODDIR/.mutation.lock/owner"
    SBX_MUTATION_OWNER_PID=$$ SBX_MUTATION_OWNER_START=$start
    SBX_MUTATION_OWNER_TOKEN=$token
    export SBX_MUTATION_OWNER_PID SBX_MUTATION_OWNER_START SBX_MUTATION_OWNER_TOKEN
}
mutation_lock_release() {
    rm -f "$MODDIR/.mutation.lock/owner"
    rmdir "$MODDIR/.mutation.lock" 2>/dev/null || :
    unset SBX_MUTATION_OWNER_PID SBX_MUTATION_OWNER_START SBX_MUTATION_OWNER_TOKEN
}
se_restore_all() { :; }
applog_wipe() {
    printf 'wipe %s\n' "$1" >> "$SBX_CALLS"
    [ "$1" != "${SBX_APPLOG_WIPE_FAIL:-}" ]
}
applog_seed() {
    printf 'seed %s\n' "$1" >> "$SBX_CALLS"
    [ "$1" != "${SBX_APPLOG_SEED_FAIL:-}" ]
}
SH
}

make_native() {
    cat > "$CASE/mod/bin/sandboxid" <<'SH'
#!/bin/sh
printf 'native %s\n' "$*" >> "$SBX_CALLS"
case "$1" in
  action-write)
    mkdir -p "$MODDIR/debug" || exit 1
    cp "$3" "$MODDIR/debug/action.$2" || exit 1
    exit "${SBX_ACTION_WRITE_RC:-0}" ;;
  targets)
    printf '%b' "${SBX_TARGETS:-}"
    exit "${SBX_TARGETS_RC:-0}" ;;
  prepare) exit "${SBX_PREPARE_RC:-0}" ;;
  commit)
    [ "${SBX_COMMIT_RC:-0}" -eq 0 ] || exit "${SBX_COMMIT_RC:-0}"
    printf 'IDENTITY_SHA256=%s\n' "${SBX_COMMIT_DIGEST:-}"
    exit 0 ;;
  abort) exit 0 ;;
  restore)
    export SBX_RESTORED=1
    touch "$MODDIR/.mock-restored"
    exit "${SBX_RESTORE_RC:-0}" ;;
  apply-props)
    if [ -e "$MODDIR/.mock-restored" ]; then exit "${SBX_REAPPLY_PROPS_RC:-0}"; fi
    exit "${SBX_APPLY_PROPS_RC:-0}" ;;
  apply-boot)
    if [ -e "$MODDIR/.mock-restored" ]; then exit "${SBX_REAPPLY_BOOT_RC:-0}"; fi
    exit "${SBX_APPLY_BOOT_RC:-0}" ;;
  verify)
    if [ "$2" = --run-id ]; then
      [ "$3" = "$SBX_ACTION_RUN_ID" ] || exit 64
      [ "$4" = --identity-sha256 ] || exit 64
      [ "$5" = "${SBX_COMMIT_DIGEST:-}" ] || exit 1
      exit "${SBX_VERIFY_RUN_RC:-0}"
    fi
    exit "${SBX_VERIFY_RESTORE_RC:-0}" ;;
  *) exit 64 ;;
esac
SH
}

make_rotate() {
    cat > "$CASE/mod/rotate_ids.sh" <<'SH'
#!/bin/sh
printf 'rotate %s\n' "$*" >> "$SBX_CALLS"
report="$MODDIR/debug/rotation.$SBX_ACTION_RUN_ID"
cat > "$report" <<EOF
run=$SBX_ACTION_RUN_ID
ssaid=ok:0
gaid=ok:0
wlan_mac=ok:0
bluetooth_mac=ok:0
device_name=ok:0
boot_count=ok:0
REBOOT_NEEDED=${SBX_ROTATE_REBOOT:-0}
FAILURES=${SBX_ROTATE_FAILURES:-0}
UNSUPPORTED=${SBX_ROTATE_UNSUPPORTED:-0}
EOF
exit "${SBX_ROTATE_RC:-0}"
SH
    cp "$CASE/mod/rotate_ids.sh" "$CASE/mod/autopif.sh"
}

make_tools() {
    cat > "$CASE/tools/id" <<'SH'
#!/bin/sh
[ "$1" = -u ] && { printf '0\n'; exit 0; }
exec /usr/bin/id "$@"
SH
    cat > "$CASE/tools/pm" <<'SH'
#!/bin/sh
printf 'pm %s\n' "$*" >> "$SBX_CALLS"
next_rc() {
  key=$1
  fallback=$2
  count_file="$MODDIR/debug/.mock-pm-$key"
  count=0
  [ ! -r "$count_file" ] || count="$(cat "$count_file")"
  count=$((count + 1))
  printf '%s\n' "$count" > "$count_file"
  eval "sequence=\${SBX_PM_${key}_SEQUENCE:-}"
  if [ -n "$sequence" ]; then
    old_ifs=$IFS
    IFS=,
    set -- $sequence
    IFS=$old_ifs
    rc=
    index=1
    for value do
      rc=$value
      [ "$index" -lt "$count" ] || break
      index=$((index + 1))
    done
    printf '%s\n' "${rc:-$fallback}"
  else
    printf '%s\n' "$fallback"
  fi
}
query_fail() {
  rc=$1
  [ "$rc" -ne 0 ] || return 1
  [ -z "${SBX_PM_FAIL_STDOUT:-}" ] || printf '%b' "$SBX_PM_FAIL_STDOUT"
  [ -z "${SBX_PM_FAIL_STDERR:-}" ] || printf '%b' "$SBX_PM_FAIL_STDERR" >&2
  exit "$rc"
}
case "$1 $2" in
  'list packages')
    shift 2
    user=0
    if [ "${1:-}" = --user ]; then
      user=1
      shift 2
    fi
    filter=${1:-}
    if [ -n "$filter" ]; then
      if [ "$filter" = "${SBX_PM_FILTER_FAIL:-}" ]; then
        rc=1
      elif [ "$user" -eq 1 ]; then
        rc="$(next_rc FILTER_USER "${SBX_PM_FILTER_USER_RC:-0}")"
      else
        rc="$(next_rc FILTER "${SBX_PM_FILTER_RC:-0}")"
      fi
      query_fail "$rc"
      printf '%b' "${SBX_PACKAGES:-}" | grep -F "package:$filter" || :
    else
      if [ "$user" -eq 1 ]; then
        rc="$(next_rc USER_QUERY "${SBX_PM_USER_QUERY_RC:-0}")"
      else
        rc="$(next_rc QUERY "${SBX_PM_QUERY_RC:-0}")"
      fi
      query_fail "$rc"
      printf '%b' "${SBX_PACKAGES:-}"
    fi ;;
  'clear --user')
    pkg=$4
    [ "$pkg" != "${SBX_PM_CLEAR_FAIL:-}" ] ;;
  *) exit 64 ;;
esac
SH
    cat > "$CASE/tools/am" <<'SH'
#!/bin/sh
printf 'am %s\n' "$*" >> "$SBX_CALLS"
pkg=$4
[ "$pkg" != "${SBX_AM_FAIL:-}" ]
SH
    printf '#!/bin/sh\nexit 0\n' > "$CASE/tools/resetprop"
    printf '#!/bin/sh\nexit 0\n' > "$CASE/tools/sleep"
}

run_case() {
    : > "$CASE/calls"
    export SBX_CALLS="$CASE/calls"
    PATH="$CASE/tools:$PATH" MODDIR="$CASE/mod" LOGFILE="$CASE/action-boot.log" \
        SBX_ACTION_RUN_ID="$RUN_ID" sh "$CASE/mod/action.sh" \
        > "$CASE/stdout" 2> "$CASE/stderr"
    CASE_RC=$?
}

result_has() { grep -q "$1" "$CASE/mod/debug/action.result"; }
stdout_has() { grep -q "$1" "$CASE/stdout"; }
call_has() { grep -q "$1" "$CASE/calls"; }

setup_case binary-missing
rm -f "$CASE/mod/bin/sandboxid"
run_case
check '[ "$CASE_RC" -eq 127 ]' 'missing binary exits 127'
check 'stdout_has "code=binary-missing"' 'missing binary emits machine result'
check '[ ! -d "$CASE/mod/.action.lock" ]' 'missing binary does not acquire Action lock'

setup_case empty-targets
SBX_TARGETS=''
export SBX_TARGETS
run_case
check '[ "$CASE_RC" -eq 64 ]' 'empty targets fail preflight with exit 64'
check 'result_has "code=target-empty"' 'empty targets persist target-empty result'
check '! call_has "native prepare"' 'empty targets perform no prepare or mutation'

setup_case invalid-targets
SBX_TARGETS_RC=64
export SBX_TARGETS_RC
run_case
check '[ "$CASE_RC" -eq 64 ]' 'invalid target parser status exits 64'
check 'result_has "code=target-invalid"' 'invalid targets persist target-invalid result'
check '! call_has "native prepare" && ! call_has "pm clear"' 'invalid targets perform no destructive work'

setup_case package-query-retry
SBX_PM_USER_QUERY_SEQUENCE='2,0'
SBX_PM_FAIL_STDOUT='cmd: Failure calling service package: Failed transaction (2147483646)\n'
export SBX_PM_USER_QUERY_SEQUENCE SBX_PM_FAIL_STDOUT
run_case
check '[ "$CASE_RC" -eq 0 ]' 'transient Binder failure recovers within the bounded query retry'
check '[ "$(grep -c "pm list packages --user 0" "$CASE/calls")" -eq 2 ]' 'transient query retries the same scoped read once before success'
check 'grep -q "label=global-user-0 attempt=1 rc=2" "$CASE/mod/debug/action.log" && grep -q "Failed transaction (2147483646)" "$CASE/mod/debug/action.log"' 'stdout-only Binder failure is retained in the Action log'
check 'result_has "status=success" && result_has "warnings=1"' 'recovered transient inventory is visible as one warning'
check 'stdout_has "TARGET.*status=cleared" && stdout_has "TARGET.*status=absent"' 'failed-attempt stdout is truncated before successful classification'

setup_case package-user-query-fallback
SBX_PM_USER_QUERY_RC=1
export SBX_PM_USER_QUERY_RC
run_case
check '[ "$CASE_RC" -eq 0 ]' 'unsupported user-scoped package query falls back to default package list'
check '[ "$(grep -c "pm list packages --user 0" "$CASE/calls")" -eq 3 ] && call_has "pm list packages$"' 'package fallback bounds scoped retries before retrying without the unsupported user option'
check 'result_has "status=success" && result_has "warnings=1"' 'package fallback is visible as a warning'

setup_case package-filtered-fallback
SBX_PM_USER_QUERY_RC=2
SBX_PM_QUERY_RC=2
SBX_PACKAGES='package:com.example.present\npackage:com.example.present.extra\npackage:com.example.absent.extra\n'
SBX_PM_FAIL_STDERR='package service busy\n'
export SBX_PM_USER_QUERY_RC SBX_PM_QUERY_RC SBX_PACKAGES SBX_PM_FAIL_STDERR
run_case
check '[ "$CASE_RC" -eq 0 ]' 'filtered target queries recover when both global inventory forms remain unavailable'
check 'call_has "pm list packages --user 0 com.example.present" && call_has "pm list packages --user 0 com.example.absent"' 'filtered fallback queries every normalized target exactly'
check 'stdout_has "TARGET.*status=cleared" && stdout_has "TARGET.*status=absent"' 'filtered fallback classifies installed and absent targets'
check '! call_has "am force-stop --user 0 com.example.present.extra" && ! call_has "pm clear --user 0 com.example.present.extra"' 'substring package output never creates an installed target'
check 'grep -q "package service busy" "$CASE/mod/debug/action.log"' 'failed-query stderr is retained before filtered recovery'

setup_case package-filtered-unknown
SBX_PM_USER_QUERY_RC=2
SBX_PM_QUERY_RC=2
SBX_PM_FILTER_FAIL=com.example.absent
SBX_PM_FAIL_STDOUT='cmd: Failure calling service package: Failed transaction (2147483646)\n'
export SBX_PM_USER_QUERY_RC SBX_PM_QUERY_RC SBX_PM_FILTER_FAIL SBX_PM_FAIL_STDOUT
run_case
check '[ "$CASE_RC" -eq 30 ]' 'one unresolved filtered target preserves fail-safe exit 30'
check 'result_has "code=package-query-failed" && ! result_has "committed=1"' 'unresolved package state is explicit and remains pre-commit'
check '! call_has "native prepare" && ! call_has "pm clear" && ! call_has "rotate " && ! call_has "wipe " && ! call_has "seed "' 'unresolved package state performs no prepare or irreversible work'

setup_case package-query-fail
SBX_PM_USER_QUERY_RC=1
SBX_PM_QUERY_RC=1
SBX_PM_FILTER_USER_RC=1
SBX_PM_FILTER_RC=1
export SBX_PM_USER_QUERY_RC SBX_PM_QUERY_RC SBX_PM_FILTER_USER_RC SBX_PM_FILTER_RC
run_case
check '[ "$CASE_RC" -eq 30 ]' 'all package queries failing exits before commit with 30'
check 'result_has "code=package-query-failed"' 'package query failure remains explicit and durable'
check '! call_has "native prepare" && ! call_has "pm clear"' 'package query failure performs no mutation'

setup_case prepare-fail
SBX_PREPARE_RC=1
export SBX_PREPARE_RC
run_case
check '[ "$CASE_RC" -eq 30 ]' 'prepare failure exits before commit with 30'
check 'result_has "code=prepare-failed"' 'prepare failure is persisted'
check '! call_has "native commit" && ! call_has "pm clear" && ! call_has "rotate "' 'prepare failure performs no irreversible work'

setup_case commit-fail
SBX_COMMIT_RC=1
export SBX_COMMIT_RC
run_case
check '[ "$CASE_RC" -eq 30 ]' 'commit failure exits 30'
check 'call_has "native abort '$RUN_ID'"' 'commit failure aborts matching transaction'
check '! call_has "pm clear" && ! call_has "rotate " && ! call_has "wipe "' 'commit failure performs no irreversible work'

setup_case apply-rollback
SBX_APPLY_BOOT_RC=1
export SBX_APPLY_BOOT_RC
run_case
check '[ "$CASE_RC" -eq 31 ]' 'apply failure with coherent restore exits 31'
check 'result_has "status=rolled-back" && result_has "code=apply-restored"' 'rollback result is explicit and durable'
check 'call_has "native restore" && call_has "native verify"' 'apply failure restores, reapplies, and verifies old identity'
check '! call_has "pm clear" && ! call_has "rotate " && ! call_has "wipe "' 'successful rollback precedes all irreversible work'

setup_case apply-degraded
SBX_APPLY_PROPS_RC=1
SBX_REAPPLY_PROPS_RC=1
export SBX_APPLY_PROPS_RC SBX_REAPPLY_PROPS_RC
run_case
check '[ "$CASE_RC" -eq 32 ]' 'unproven rollback exits 32'
check 'result_has "status=degraded" && result_has "code=rollback-incomplete"' 'degraded rollback is durable and explicit'
check '! call_has "pm clear" && ! call_has "rotate " && ! call_has "wipe "' 'degraded rollback still avoids irreversible work'

setup_case success-accounting
run_case
check '[ "$CASE_RC" -eq 0 ]' 'complete mocked Action exits 0'
check 'stdout_has "TARGET.*status=cleared" && stdout_has "TARGET.*status=absent"' 'installed and absent target statuses are both emitted'
check 'stdout_has "APPLOG.*status=seeded" && stdout_has "APPLOG.*status=absent"' 'AppLog installed and absent statuses are both emitted'
check 'call_has "pm clear --user 0 com.example.present"' 'only installed package is cleared'
check '! call_has "pm clear --user 0 com.example.absent"' 'absent package is never cleared'
check 'result_has "run=$RUN_ID" && result_has "status=success" && result_has "exit=0"' 'terminal result is correlated and durable'
check 'grep -q "^run=$RUN_ID$" "$CASE/mod/debug/action.state" && grep -q "^status=terminal$" "$CASE/mod/debug/action.state"' 'terminal state is correlated and durable'
check '[ ! -d "$CASE/mod/.action.lock" ]' 'Action lock is released after success'
check '[ ! -d "$CASE/mod/.mutation.lock" ]' 'global mutation lock is released after success'
check 'call_has "native verify --run-id $RUN_ID --identity-sha256 $SBX_COMMIT_DIGEST"' 'final verify binds run and exact committed digest'

setup_case force-stop-fail
SBX_AM_FAIL=com.example.present
export SBX_AM_FAIL
run_case
check '[ "$CASE_RC" -eq 0 ]' 'force-stop failure does not block a successful package clear'
check 'call_has "am force-stop --user 0 com.example.present" && call_has "pm clear --user 0 com.example.present"' 'package clear is attempted independently after force-stop failure'
check 'result_has "status=success" && result_has "warnings=1"' 'force-stop failure is reported as a warning when clear succeeds'

setup_case postcommit-partial
SBX_PM_CLEAR_FAIL=com.example.present
export SBX_PM_CLEAR_FAIL
run_case
check '[ "$CASE_RC" -eq 20 ]' 'post-commit target failure exits partial 20'
check 'result_has "status=partial" && result_has "code=forward-recovery"' 'post-commit failure keeps new identity with forward recovery'
check '! call_has "native restore"' 'post-commit partial never rolls canonical state back'
check 'call_has "rotate all --from-identity" && call_has "seed com.example.present"' 'forward recovery continues rotation and AppLog accounting'

setup_case rotate-unsupported
SBX_ROTATE_RC=3
SBX_ROTATE_UNSUPPORTED=1
export SBX_ROTATE_RC SBX_ROTATE_UNSUPPORTED
run_case
check '[ "$CASE_RC" -eq 20 ]' 'required unsupported rotation returns partial 20'
check 'stdout_has "ROTATE.*component=wlan_mac.*status=ok"' 'component rotation protocol is emitted'

setup_case reboot
SBX_ROTATE_REBOOT=1
export SBX_ROTATE_REBOOT
run_case
check '[ "$CASE_RC" -eq 10 ]' 'successful Action requiring reboot exits 10'
check 'result_has "reboot=1" && result_has "code=reboot-required"' 'reboot requirement is durable'

setup_case live-lock
mkdir -p "$CASE/mod/.mutation.lock"
start=$(awk '{print $22; exit}' "/proc/$$/stat")
printf 'version=1\nkind=standalone\npid=%s\nproc_start=%s\ntoken=%s\n' \
    "$$" "$start" 22222222222222222222222222222222 \
    > "$CASE/mod/.mutation.lock/owner"
run_case
check '[ "$CASE_RC" -eq 75 ]' 'live global mutation lock returns busy 75'
check 'stdout_has "status=busy" && stdout_has "code=mutation-active"' 'live global lock emits busy result'
check '! call_has "native prepare"' 'busy global owner performs no transaction work'

setup_case stale-lock
mkdir -p "$CASE/mod/.mutation.lock"
printf 'version=1\nkind=standalone\npid=999999\nproc_start=1\ntoken=%s\n' \
    33333333333333333333333333333333 > "$CASE/mod/.mutation.lock/owner"
run_case
check '[ "$CASE_RC" -eq 0 ]' 'dead global owner is reclaimed'
check '[ ! -d "$CASE/mod/.mutation.lock" ]' 'reclaimed global lock is released after completion'

sh -n "$ROOT/action.sh" "$WORK/binary-missing/mod/helpers.sh" \
    "$WORK/binary-missing/mod/bin/resetprop-rs" 2>/dev/null || failures=$((failures + 1))

printf '\n%d checks, %d failures\n' "$checks" "$failures"
exit "$failures"
