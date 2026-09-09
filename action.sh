#!/system/bin/sh

MODDIR="${MODDIR:-${0%/*}}"
BIN="${SBX_BIN:-$MODDIR/bin/sandboxid}"
AUTOPIF="$MODDIR/autopif.sh"
ROTATE="$MODDIR/rotate_ids.sh"
TARGET_FILE="$MODDIR/target.txt"
ACTION_LOCK="$MODDIR/.action.lock"
DEBUG_DIR="$MODDIR/debug"
ACTION_LOG="$DEBUG_DIR/action.log"
LOGFILE="${LOGFILE:-/cache/sandboxid-boot.log}"
[ -w "${LOGFILE%/*}" ] || LOGFILE="$DEBUG_DIR/action-boot.log"

PROTO=SBX_ACTION_V1
RUN_ID="${SBX_ACTION_RUN_ID:-}"
STAGE=init
STARTED_UTC=""
COMMITTED=0
IRREVERSIBLE=0
WARNINGS=0
FAILURES=0
REBOOT=0
ACTION_LOCK_HELD=0
ACTION_OWNER_START=""
GLOBAL_LOCK_HELD=0
COMMITTED_IDENTITY_SHA256=""
TERMINAL=0
PERSISTENCE_FAILED=0
HELPERS_READY=0
TARGETS_FILE=""
INSTALLED_TARGETS_FILE=""
ABSENT_TARGETS_FILE=""
PACKAGE_LIST_FILE=""

if [ "$(id -u 2>/dev/null)" != 0 ]; then
    _bootstrap_run="${SBX_ACTION_RUN_ID:-00000000000000000000000000000000}"
    if [ "${#_bootstrap_run}" -ne 32 ]; then
        _bootstrap_run=00000000000000000000000000000000
    fi
    case "$_bootstrap_run" in
        *[!0-9a-f]*) _bootstrap_run=00000000000000000000000000000000 ;;
    esac
    printf '%s\tBEGIN\trun=%s\n' "$PROTO" "$_bootstrap_run"
    printf '%s\n' '[ERR] Action harus dijalankan sebagai root.' >&2
    printf '%s\tRESULT\tstatus=failed\treboot=0\twarnings=0\tfailures=1\texit=64\trun=%s\tcode=root-required\n' \
        "$PROTO" "$_bootstrap_run"
    exit 64
fi

mkdir -p "$DEBUG_DIR" 2>/dev/null
chmod 0755 "$DEBUG_DIR" 2>/dev/null
if [ ! -d "$DEBUG_DIR" ] || [ ! -w "$DEBUG_DIR" ]; then
    printf '%s\n' '[ERR] debug directory is unavailable' >&2
    exit 32
fi
: >> "$ACTION_LOG" 2>/dev/null
: >> "$LOGFILE" 2>/dev/null

if [ -r "$MODDIR/helpers.sh" ]; then
    . "$MODDIR/helpers.sh"
    HELPERS_READY=1
fi

resolve_bin() {
    if command -v sbx_bin >/dev/null 2>&1; then
        _resolved="$(sbx_bin 2>/dev/null)"
        [ -x "$_resolved" ] && BIN="$_resolved"
    fi
    [ -x "$BIN" ]
}

now_utc() { date +%s 2>/dev/null || printf '0\n'; }

make_run_id() {
    _run="$(od -An -N16 -tx1 /dev/urandom 2>/dev/null | tr -d ' \n')"
    case "$_run" in
        [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f])
            printf '%s\n' "$_run"
            return ;;
    esac
    _p1=$(( $$ & 0xffffffff ))
    _p2=$(( $(now_utc) & 0xffffffff ))
    _p3=$(( ( $$ * 1103515245 ) & 0xffffffff ))
    _p4=$(( ( $(now_utc) + $$ ) & 0xffffffff ))
    _run="$(printf '%08x%08x%08x%08x\n' "$_p1" "$_p2" "$_p3" "$_p4")"
    if valid_run_id "$_run"; then
        printf '%s\n' "$_run"
    else
        printf '%s\n' 00000000000000000000000000000000
    fi
}

valid_run_id() {
    [ "${#1}" -eq 32 ] || return 1
    case "$1" in *[!0-9a-f]*) return 1 ;; esac
    return 0
}

valid_sha256() {
    [ "${#1}" -eq 64 ] || return 1
    case "$1" in *[!0-9a-f]*) return 1 ;; esac
    return 0
}

b64() {
    if command -v base64 >/dev/null 2>&1; then
        printf '%s' "$1" | base64 | tr -d '\r\n'
    else
        printf '%s' "$1" | toybox base64 | tr -d '\r\n'
    fi
}

say() {
    printf '%s\n' "$*" | tee -a "$ACTION_LOG" "$LOGFILE"
}

proto() {
    _record="$(protocol_record "$1")"
    printf '%s\n' "$_record" | tee -a "$ACTION_LOG" "$LOGFILE"
}

protocol_record() {
    printf '%s\t%b\n' "$PROTO" "$1"
}

publish_action_file() {
    _kind="$1"
    _body="$2"
    _source="$DEBUG_DIR/.action-${_kind}.${RUN_ID}.$$"
    umask 077
    if ! printf '%s\n' "$_body" > "$_source" 2>/dev/null ||
       ! chmod 0600 "$_source" 2>/dev/null; then
        rm -f "$_source" 2>/dev/null
        umask 022
        return 1
    fi
    umask 022
    "$BIN" action-write "$_kind" "$_source" </dev/null >/dev/null 2>&1
    _publish_rc=$?
    rm -f "$_source" 2>/dev/null
    return "$_publish_rc"
}

atomic_text() {
    _dst="$1"
    _body="$2"
    _tmp="${_dst}.tmp.$$"
    umask 077
    if ! printf '%s\n' "$_body" > "$_tmp" 2>/dev/null; then
        rm -f "$_tmp" 2>/dev/null
        umask 022
        return 1
    fi
    chmod 0644 "$_tmp" 2>/dev/null || {
        rm -f "$_tmp" 2>/dev/null
        umask 022
        return 1
    }
    if ! mv -f "$_tmp" "$_dst" 2>/dev/null; then
        rm -f "$_tmp" 2>/dev/null
        umask 022
        return 1
    fi
    umask 022
    return 0
}

state_write() {
    _status="$1"
    _extra="${2:-}"
    _now="$(now_utc)"
    _body="version=1
run=$RUN_ID
stage=$STAGE
status=$_status
started_utc=$STARTED_UTC
updated_utc=$_now
committed=$COMMITTED
irreversible=$IRREVERSIBLE
warnings=$WARNINGS
failures=$FAILURES
reboot=$REBOOT"
    [ -n "$_extra" ] && _body="$_body
$_extra"
    publish_action_file state "$_body"
}

persist_running_state() {
    if state_write running; then
        return 0
    fi
    PERSISTENCE_FAILED=1
    if [ "$WARNINGS" -eq 0 ] || [ "${LAST_PERSISTENCE_STAGE:-}" != "$STAGE" ]; then
        LAST_PERSISTENCE_STAGE="$STAGE"
        warning "Progress Action tidak bisa dipersistenkan pada tahap $STAGE."
    fi
    return 1
}

stage() {
    STAGE="$1"
    persist_running_state || :
    proto "STAGE\t$STAGE\tBEGIN\tcode=running"
    say "==> $2"
}

stage_done() {
    _status="$1"
    _code="$2"
    proto "STAGE\t$STAGE\t$_status\tcode=$_code"
    persist_running_state || :
}

warning() {
    WARNINGS=$((WARNINGS + 1))
    say "[WARN] $*"
}

failure() {
    FAILURES=$((FAILURES + 1))
    say "[ERR] $*"
}

run_capture() {
    _out="$DEBUG_DIR/.action-command.$$"
    "$@" </dev/null > "$_out" 2>&1
    CMD_RC=$?
    CMD_IDENTITY_SHA256="$(awk -F= '$1=="IDENTITY_SHA256"{print $2;exit}' "$_out" 2>/dev/null)"
    [ -s "$_out" ] && tee -a "$ACTION_LOG" "$LOGFILE" < "$_out"
    rm -f "$_out" 2>/dev/null
    return "$CMD_RC"
}

lock_owner_alive() {
    _owner_file_live "$ACTION_LOCK/owner"
}

property_backend_available() {
    [ -x "$MODDIR/bin/resetprop-rs" ] && return 0
    command -v resetprop >/dev/null 2>&1 && return 0
    command -v resetprop-rs >/dev/null 2>&1
}

lock_age_seconds() {
    _now="$(now_utc)"
    _stamp="$(stat -c %Y "$ACTION_LOCK" 2>/dev/null)"
    case "$_now:$_stamp" in
        *[!0-9:]*|:*|*:) return 1 ;;
    esac
    _age=$((_now - _stamp))
    [ "$_age" -ge 0 ] || _age=0
    printf '%s\n' "$_age"
}

acquire_action_lock() {
    if mkdir "$ACTION_LOCK" 2>/dev/null; then
        :
    elif [ ! -r "$ACTION_LOCK/owner" ]; then
        _lock_age="$(lock_age_seconds 2>/dev/null)" || return 75
        [ "$_lock_age" -ge 300 ] || return 75
        _stale="${ACTION_LOCK}.stale.$$"
        mv "$ACTION_LOCK" "$_stale" 2>/dev/null || return 75
        rm -rf "$_stale" 2>/dev/null || return 1
        mkdir "$ACTION_LOCK" 2>/dev/null || return 75
    elif lock_owner_alive; then
        return 75
    else
        _stale="${ACTION_LOCK}.stale.$$"
        mv "$ACTION_LOCK" "$_stale" 2>/dev/null || return 75
        rm -rf "$_stale" 2>/dev/null || return 1
        mkdir "$ACTION_LOCK" 2>/dev/null || return 75
    fi
    chmod 0700 "$ACTION_LOCK" 2>/dev/null || {
        rmdir "$ACTION_LOCK" 2>/dev/null
        return 1
    }
    ACTION_LOCK_HELD=1
    ACTION_OWNER_START="$(proc_start_ticks "$$" 2>/dev/null)"
    case "$ACTION_OWNER_START" in ''|*[!0-9]*)
        rmdir "$ACTION_LOCK" 2>/dev/null
        ACTION_LOCK_HELD=0
        return 1 ;;
    esac
    if ! atomic_text "$ACTION_LOCK/owner" "version=1
kind=action
pid=$$
proc_start=$ACTION_OWNER_START
run=$RUN_ID
token=$SBX_MUTATION_OWNER_TOKEN"; then
        rm -rf "$ACTION_LOCK" 2>/dev/null
        ACTION_LOCK_HELD=0
        return 1
    fi
    return 0
}

release_action_lock() {
    [ "$ACTION_LOCK_HELD" -eq 1 ] || return 0
    _owned_run="$(awk -F= '$1=="run"{print $2;exit}' "$ACTION_LOCK/owner" 2>/dev/null)"
    _owned_pid="$(awk -F= '$1=="pid"{print $2;exit}' "$ACTION_LOCK/owner" 2>/dev/null)"
    _owned_start="$(awk -F= '$1=="proc_start"{print $2;exit}' "$ACTION_LOCK/owner" 2>/dev/null)"
    _owned_token="$(awk -F= '$1=="token"{print $2;exit}' "$ACTION_LOCK/owner" 2>/dev/null)"
    if [ "$_owned_run" = "$RUN_ID" ] && [ "$_owned_pid" = "$$" ] &&
       [ "$_owned_start" = "$ACTION_OWNER_START" ] &&
       [ "$_owned_token" = "${SBX_MUTATION_OWNER_TOKEN:-}" ]; then
        rm -f "$ACTION_LOCK/owner" 2>/dev/null
        rmdir "$ACTION_LOCK" 2>/dev/null
    fi
    ACTION_LOCK_HELD=0
    ACTION_OWNER_START=""
}

release_global_lock() {
    [ "$GLOBAL_LOCK_HELD" -eq 1 ] || return 0
    mutation_lock_release
    GLOBAL_LOCK_HELD=0
}

finish() {
    _exit="$1"
    _status="$2"
    _code="$3"
    command -v se_restore_all >/dev/null 2>&1 && se_restore_all
    TERMINAL=1
    STAGE=result
    if [ "$PERSISTENCE_FAILED" -ne 0 ] && [ "$_exit" -ne 32 ]; then
        _exit=32
        _status=degraded
        _code=persistence-failed
        FAILURES=$((FAILURES + 1))
    fi
    _record="$(protocol_record "RESULT\tstatus=$_status\treboot=$REBOOT\twarnings=$WARNINGS\tfailures=$FAILURES\texit=$_exit\trun=$RUN_ID\tcode=$_code")"
    _result_ok=1
    _state_ok=1
    if ! publish_action_file result "$_record"; then
        _result_ok=0
    fi
    if ! state_write terminal "exit=$_exit
result=$_status
code=$_code
finished_utc=$(now_utc)"; then
        _state_ok=0
    fi
    if [ "$_result_ok" -eq 0 ] || [ "$_state_ok" -eq 0 ]; then
        if [ "$_exit" -ne 32 ]; then
            _exit=32
            _status=degraded
            _code=persistence-failed
            FAILURES=$((FAILURES + 1))
            _record="$(protocol_record "RESULT\tstatus=$_status\treboot=$REBOOT\twarnings=$WARNINGS\tfailures=$FAILURES\texit=$_exit\trun=$RUN_ID\tcode=$_code")"
            publish_action_file result "$_record" >/dev/null 2>&1 || :
            state_write terminal "exit=$_exit
result=$_status
code=$_code
finished_utc=$(now_utc)" >/dev/null 2>&1 || :
        else
            FAILURES=$((FAILURES + 1))
            _code="${_code}+persistence-failed"
            _record="$(protocol_record "RESULT\tstatus=degraded\treboot=$REBOOT\twarnings=$WARNINGS\tfailures=$FAILURES\texit=32\trun=$RUN_ID\tcode=$_code")"
        fi
    fi
    printf '%s\n' "$_record" | tee -a "$ACTION_LOG" "$LOGFILE"
    rm -f "$TARGETS_FILE" "$INSTALLED_TARGETS_FILE" "$ABSENT_TARGETS_FILE" \
        "$PACKAGE_LIST_FILE" 2>/dev/null
    release_action_lock
    release_global_lock
    trap - EXIT INT TERM HUP
    exit "$_exit"
}

on_signal() {
    _signal_rc="$1"
    command -v se_restore_all >/dev/null 2>&1 && se_restore_all
    failure "Action dihentikan saat tahap $STAGE (signal rc=$_signal_rc)."
    if [ "$COMMITTED" -eq 0 ] && valid_run_id "$RUN_ID" && [ -x "$BIN" ]; then
        "$BIN" abort "$RUN_ID" </dev/null >/dev/null 2>&1 || true
    fi
    finish 32 degraded interrupted
}

cleanup() {
    [ "$TERMINAL" -eq 1 ] && return
    command -v se_restore_all >/dev/null 2>&1 && se_restore_all
    rm -f "$TARGETS_FILE" "$INSTALLED_TARGETS_FILE" "$ABSENT_TARGETS_FILE" \
        "$PACKAGE_LIST_FILE" 2>/dev/null
    release_action_lock
    release_global_lock
}
trap cleanup EXIT
trap 'on_signal 130' INT
trap 'on_signal 143' TERM
trap 'on_signal 129' HUP

if [ -n "$RUN_ID" ] && ! valid_run_id "$RUN_ID"; then
    RUN_ID=""
fi
[ -n "$RUN_ID" ] || RUN_ID="$(make_run_id)"
SBX_ACTION_RUN_ID="$RUN_ID"
export SBX_ACTION_RUN_ID
STARTED_UTC="$(now_utc)"

if ! resolve_bin; then
    proto "BEGIN\trun=$RUN_ID"
    failure "Binary native SandboxID tidak tersedia; hasil tidak dapat dipersistenkan."
    proto "RESULT\tstatus=failed\treboot=0\twarnings=$WARNINGS\tfailures=$FAILURES\texit=127\trun=$RUN_ID\tcode=binary-missing"
    trap - EXIT INT TERM HUP
    exit 127
fi

if [ "$HELPERS_READY" -ne 1 ] ||
   ! command -v mutation_lock_acquire >/dev/null 2>&1 ||
   ! command -v mutation_lock_release >/dev/null 2>&1 ||
   ! command -v proc_start_ticks >/dev/null 2>&1; then
    proto "BEGIN\trun=$RUN_ID"
    failure "helpers.sh atau kontrak global mutation lock tidak tersedia."
    proto "RESULT\tstatus=failed\treboot=0\twarnings=$WARNINGS\tfailures=$FAILURES\texit=127\trun=$RUN_ID\tcode=helpers-missing"
    trap - EXIT INT TERM HUP
    exit 127
fi
if mutation_lock_acquire action "$RUN_ID"; then
    GLOBAL_LOCK_HELD=1
else
    _global_rc=$?
    [ "$_global_rc" -eq 75 ] || _global_rc=32
    STAGE=busy
    proto "BEGIN\trun=$RUN_ID"
    proto "RESULT\tstatus=busy\treboot=0\twarnings=0\tfailures=0\texit=$_global_rc\trun=$RUN_ID\tcode=mutation-active"
    trap - EXIT INT TERM HUP
    exit "$_global_rc"
fi

if acquire_action_lock; then
    :
else
    _action_rc=$?
    [ "$_action_rc" -eq 75 ] || _action_rc=32
    STAGE=busy
    proto "BEGIN\trun=$RUN_ID"
    proto "RESULT\tstatus=busy\treboot=0\twarnings=0\tfailures=0\texit=$_action_rc\trun=$RUN_ID\tcode=action-active"
    release_global_lock
    trap - EXIT INT TERM HUP
    exit "$_action_rc"
fi

if ! state_write running; then
    PERSISTENCE_FAILED=1
    warning "Progress Action tidak bisa dipersistenkan."
fi
proto "BEGIN\trun=$RUN_ID"
say ""
say "SandboxID — satu klik identitas dan privasi baru"
say "Run: $RUN_ID"

stage preflight "Memeriksa prasyarat dan target"
if [ ! -r "$TARGET_FILE" ]; then
    failure "target.txt tidak dapat dibaca."
    stage_done FAIL target-unreadable
    finish 64 failed target-unreadable
fi
if [ "$HELPERS_READY" -ne 1 ] ||
   ! command -v se_restore_all >/dev/null 2>&1 ||
   ! command -v applog_wipe >/dev/null 2>&1 ||
   ! command -v applog_seed >/dev/null 2>&1; then
    failure "helpers.sh atau fungsi AppLog wajib tidak tersedia."
    stage_done FAIL helpers-missing
    finish 127 failed helpers-missing
fi
if [ ! -r "$ROTATE" ]; then
    failure "rotate_ids.sh tidak tersedia sebelum commit."
    stage_done FAIL rotate-missing
    finish 127 failed rotate-missing
fi
if [ -f "$MODDIR/enable_remote_refresh" ] && [ ! -r "$AUTOPIF" ]; then
    failure "Remote refresh aktif tetapi autopif.sh tidak dapat dibaca."
    stage_done FAIL refresh-helper-missing
    finish 127 failed refresh-helper-missing
fi
TARGETS_FILE="$DEBUG_DIR/.action-targets.$RUN_ID"
_targets_err="$DEBUG_DIR/.action-targets-error.$$"
"$BIN" targets --packages </dev/null > "$TARGETS_FILE" 2> "$_targets_err"
_targets_rc=$?
[ -s "$TARGETS_FILE" ] && tee -a "$ACTION_LOG" "$LOGFILE" < "$TARGETS_FILE"
[ -s "$_targets_err" ] && tee -a "$ACTION_LOG" "$LOGFILE" < "$_targets_err"
rm -f "$_targets_err" 2>/dev/null
if [ "$_targets_rc" -ne 0 ]; then
    rm -f "$TARGETS_FILE" 2>/dev/null
    failure "Daftar target tidak valid."
    stage_done FAIL target-invalid
    finish 64 failed target-invalid
fi
if [ ! -s "$TARGETS_FILE" ]; then
    rm -f "$TARGETS_FILE" 2>/dev/null
    failure "target.txt tidak memiliki package efektif."
    stage_done FAIL target-empty
    finish 64 failed target-empty
fi
_mode="$(tr -d ' \t\r\n' < "$MODDIR/identity.mode" 2>/dev/null)"
if [ "$_mode" = locked ]; then
    rm -f "$TARGETS_FILE" 2>/dev/null
    failure "identity.mode terkunci; Action tidak akan membukanya diam-diam."
    stage_done FAIL identity-locked
    finish 64 failed identity-locked
fi
if ! property_backend_available; then
    failure "Backend resetprop tidak tersedia sebelum commit."
    stage_done FAIL property-backend-missing
    finish 127 failed property-backend-missing
fi
if ! command -v pm >/dev/null 2>&1 || ! command -v am >/dev/null 2>&1; then
    rm -f "$TARGETS_FILE" 2>/dev/null
    failure "pm/am tidak tersedia untuk reset target."
    stage_done FAIL framework-tools-missing
    finish 127 failed framework-tools-missing
fi
PACKAGE_LIST_FILE="$DEBUG_DIR/.action-package-list.$RUN_ID"
INSTALLED_TARGETS_FILE="$DEBUG_DIR/.action-installed.$RUN_ID"
ABSENT_TARGETS_FILE="$DEBUG_DIR/.action-absent.$RUN_ID"
if pm list packages --user 0 </dev/null > "$PACKAGE_LIST_FILE" 2>> "$ACTION_LOG"; then
    :
else
    : > "$PACKAGE_LIST_FILE" || {
        rm -f "$TARGETS_FILE" "$PACKAGE_LIST_FILE" 2>/dev/null
        failure "Daftar package sementara tidak dapat dibuat."
        stage_done FAIL target-state-unwritable
        finish 30 failed target-state-unwritable
    }
    if pm list packages </dev/null > "$PACKAGE_LIST_FILE" 2>> "$ACTION_LOG"; then
        warning "Query package untuk user 0 gagal; menggunakan daftar package default."
    else
        rm -f "$TARGETS_FILE" "$PACKAGE_LIST_FILE" 2>/dev/null
        failure "Package Manager tidak dapat membaca daftar package."
        stage_done FAIL package-query-failed
        finish 30 failed package-query-failed
    fi
fi
: > "$INSTALLED_TARGETS_FILE" || {
    rm -f "$TARGETS_FILE" "$PACKAGE_LIST_FILE" 2>/dev/null
    failure "Daftar target terpasang tidak dapat dibuat."
    stage_done FAIL target-state-unwritable
    finish 30 failed target-state-unwritable
}
: > "$ABSENT_TARGETS_FILE" || {
    rm -f "$TARGETS_FILE" "$PACKAGE_LIST_FILE" "$INSTALLED_TARGETS_FILE" 2>/dev/null
    failure "Daftar target absent tidak dapat dibuat."
    stage_done FAIL target-state-unwritable
    finish 30 failed target-state-unwritable
}
if ! awk -v installed="$INSTALLED_TARGETS_FILE" -v absent="$ABSENT_TARGETS_FILE" '
    NR == FNR {
        if (index($0, "package:") == 1) packages[substr($0, 9)] = 1
        next
    }
    $0 != "" {
        destination = (($0 in packages) ? installed : absent)
        print $0 >> destination
        if (close(destination) != 0) exit 1
    }
' "$PACKAGE_LIST_FILE" "$TARGETS_FILE"; then
    rm -f "$TARGETS_FILE" "$PACKAGE_LIST_FILE" "$INSTALLED_TARGETS_FILE" "$ABSENT_TARGETS_FILE" 2>/dev/null
    failure "Klasifikasi target gagal dipersistenkan."
    stage_done FAIL target-state-unwritable
    finish 30 failed target-state-unwritable
fi
rm -f "$PACKAGE_LIST_FILE" 2>/dev/null
stage_done OK ready

stage refresh "Memeriksa pembaruan persona opsional"
if [ -f "$MODDIR/enable_remote_refresh" ] && [ -r "$AUTOPIF" ]; then
    if MODDIR="$MODDIR" SBX_BIN="$BIN" sh "$AUTOPIF" refresh >> "$ACTION_LOG" 2>&1; then
        stage_done OK refresh-complete
    else
        _refresh_rc=$?
        warning "Refresh persona gagal (rc=$_refresh_rc); sumber offline tetap digunakan."
        stage_done WARN offline-fallback
    fi
else
    stage_done SKIP disabled
fi

stage prepare "Menyiapkan snapshot identitas tanpa mutasi perangkat"
if ! run_capture "$BIN" prepare "$RUN_ID"; then
    _prepare_rc=$CMD_RC
    rm -f "$TARGETS_FILE" 2>/dev/null
    failure "Prepare gagal (rc=$_prepare_rc)."
    stage_done FAIL prepare-failed
    case "$_prepare_rc" in
        64) finish 64 failed prepare-invalid ;;
        75) finish 75 busy state-busy ;;
        32) finish 32 degraded prepare-degraded ;;
        *)  finish 30 failed prepare-failed ;;
    esac
fi
stage_done OK prepared

stage commit "Mempublikasikan identitas dan overlay tervalidasi"
run_capture "$BIN" commit "$RUN_ID"
_commit_rc=$CMD_RC
COMMITTED_IDENTITY_SHA256="$CMD_IDENTITY_SHA256"
valid_sha256 "$COMMITTED_IDENTITY_SHA256" || COMMITTED_IDENTITY_SHA256=""
case "$_commit_rc" in
    0)
        if [ -z "$COMMITTED_IDENTITY_SHA256" ]; then
            failure "Commit tidak mengembalikan digest identitas canonical."
            stage_done FAIL commit-digest-missing
            finish 32 degraded commit-unproven
        fi
        COMMITTED=1
        stage_done OK committed ;;
    20)
        if [ -n "$COMMITTED_IDENTITY_SHA256" ] &&
           run_capture "$BIN" verify --run-id "$RUN_ID" \
               --identity-sha256 "$COMMITTED_IDENTITY_SHA256"; then
            COMMITTED=1
            warning "Identitas sudah commit, tetapi cleanup transaksi belum lengkap."
            stage_done WARN committed-cleanup-incomplete
        else
            rm -f "$TARGETS_FILE" 2>/dev/null
            failure "Commit dilaporkan parsial dan identitas run tidak dapat diverifikasi."
            stage_done FAIL commit-unproven
            finish 32 degraded commit-unproven
        fi ;;
    *)
        "$BIN" abort "$RUN_ID" </dev/null >> "$ACTION_LOG" 2>&1 || true
        rm -f "$TARGETS_FILE" 2>/dev/null
        failure "Commit gagal (rc=$_commit_rc); mutasi target belum dimulai."
        stage_done FAIL commit-failed
        case "$_commit_rc" in
            75) finish 75 busy state-busy ;;
            32) finish 32 degraded commit-degraded ;;
            *)  finish 30 failed commit-failed ;;
        esac ;;
esac

stage apply "Menerapkan properti dan Settings framework"
_apply_failed=0
_apply_degraded=0
run_capture "$BIN" apply-props || {
    _apply_rc=$CMD_RC
    [ "$_apply_rc" -eq 32 ] && _apply_degraded=1
    _apply_failed=1
}
run_capture "$BIN" apply-boot || {
    _apply_rc=$CMD_RC
    [ "$_apply_rc" -eq 32 ] && _apply_degraded=1
    _apply_failed=1
}
if [ "$_apply_failed" -ne 0 ]; then
    failure "Penerapan identitas baru gagal sebelum mutasi target; mencoba restore."
    _restore_ok=1
    run_capture "$BIN" restore
    _restore_rc=$CMD_RC
    case "$_restore_rc" in 0|20) : ;; *) _restore_ok=0 ;; esac
    if [ "$_restore_ok" -eq 1 ] &&
       run_capture "$BIN" apply-props &&
       run_capture "$BIN" apply-boot &&
       run_capture "$BIN" verify; then
        COMMITTED=0
        rm -f "$TARGETS_FILE" 2>/dev/null
        stage_done FAIL restored
        finish 31 rolled-back apply-restored
    fi
    rm -f "$TARGETS_FILE" 2>/dev/null
    stage_done FAIL rollback-incomplete
    finish 32 degraded rollback-incomplete
fi
stage_done OK applied

stage targets "Mereset data setiap aplikasi target terpasang"
_stage_failures=$FAILURES
while IFS= read -r _pkg || [ -n "$_pkg" ]; do
    [ -n "$_pkg" ] || continue
    _pkg64="$(b64 "$_pkg")"
    proto "TARGET\tpkg=$_pkg64\tstatus=absent"
done < "$ABSENT_TARGETS_FILE"
while IFS= read -r _pkg || [ -n "$_pkg" ]; do
    [ -n "$_pkg" ] || continue
    _pkg64="$(b64 "$_pkg")"
    IRREVERSIBLE=1
    _stop_rc=0
    am force-stop --user 0 "$_pkg" </dev/null >/dev/null 2>&1 || _stop_rc=$?
    if pm clear --user 0 "$_pkg" </dev/null >/dev/null 2>&1; then
        if [ "$_stop_rc" -ne 0 ]; then
            warning "Force-stop target $_pkg gagal (rc=$_stop_rc), tetapi data berhasil direset."
        fi
        proto "TARGET\tpkg=$_pkg64\tstatus=cleared"
    else
        failure "Reset target $_pkg gagal."
        proto "TARGET\tpkg=$_pkg64\tstatus=failed"
    fi
done < "$INSTALLED_TARGETS_FILE"
[ "$FAILURES" -eq "$_stage_failures" ] && stage_done OK targets-complete || stage_done WARN targets-partial

stage rotate "Menerapkan nilai rotasi dari snapshot yang sudah commit"
_rotation_report="$DEBUG_DIR/rotation.$RUN_ID"
if [ ! -r "$ROTATE" ]; then
    failure "rotate_ids.sh tidak tersedia."
    stage_done FAIL rotate-missing
else
    IRREVERSIBLE=1
    MODDIR="$MODDIR" LOGFILE="$LOGFILE" SBX_ACTION_RUN_ID="$RUN_ID" \
        sh "$ROTATE" all --from-identity >> "$ACTION_LOG" 2>&1
    _rotate_rc=$?
    _report_run="$(awk -F= '$1=="run"{print $2;exit}' "$_rotation_report" 2>/dev/null)"
    _rotate_reboot="$(awk -F= '$1=="REBOOT_NEEDED"{print $2;exit}' "$_rotation_report" 2>/dev/null)"
    _rotate_failures="$(awk -F= '$1=="FAILURES"{print $2;exit}' "$_rotation_report" 2>/dev/null)"
    _rotate_unsupported="$(awk -F= '$1=="UNSUPPORTED"{print $2;exit}' "$_rotation_report" 2>/dev/null)"
    case "$_rotate_failures" in ''|*[!0-9]*) _rotate_failures=-1 ;; esac
    case "$_rotate_unsupported" in ''|*[!0-9]*) _rotate_unsupported=-1 ;; esac
    if [ "$_report_run" != "$RUN_ID" ] || [ "$_rotate_failures" -lt 0 ] ||
       [ "$_rotate_unsupported" -lt 0 ]; then
        failure "Laporan rotasi run tidak valid/tidak lengkap (rc=$_rotate_rc)."
        stage_done WARN rotate-report-invalid
    else
        [ "$_rotate_reboot" = 1 ] && REBOOT=1
        while IFS='=' read -r _component _outcome; do
            case "$_component:$_outcome" in
                ssaid:*|gaid:*|wlan_mac:*|bluetooth_mac:*|device_name:*|boot_count:*)
                    proto "ROTATE\tcomponent=$_component\tstatus=${_outcome%%:*}\trc=${_outcome#*:}" ;;
            esac
        done < "$_rotation_report"
        if [ "$_rotate_rc" -ne 0 ] || [ "$_rotate_failures" -gt 0 ]; then
            failure "Rotasi perangkat parsial/gagal (rc=$_rotate_rc, failures=$_rotate_failures)."
            stage_done WARN rotate-partial
        elif [ "$_rotate_unsupported" -gt 0 ]; then
            warning "Rotasi selesai dengan $_rotate_unsupported komponen tidak didukung."
            stage_done WARN rotate-unsupported
        else
            stage_done OK rotated
        fi
    fi
fi

stage applog "Menghapus dan menanam ulang cache AppLog per target"
_stage_failures=$FAILURES
while IFS= read -r _pkg || [ -n "$_pkg" ]; do
    [ -n "$_pkg" ] || continue
    _pkg64="$(b64 "$_pkg")"
    proto "APPLOG\tpkg=$_pkg64\tstatus=absent"
done < "$ABSENT_TARGETS_FILE"
while IFS= read -r _pkg || [ -n "$_pkg" ]; do
    [ -n "$_pkg" ] || continue
    _pkg64="$(b64 "$_pkg")"
    IRREVERSIBLE=1
    if applog_wipe "$_pkg" >> "$ACTION_LOG" 2>&1 &&
       applog_seed "$_pkg" >> "$ACTION_LOG" 2>&1; then
        proto "APPLOG\tpkg=$_pkg64\tstatus=seeded"
    else
        failure "AppLog $_pkg tidak selesai koheren."
        proto "APPLOG\tpkg=$_pkg64\tstatus=failed"
    fi
done < "$INSTALLED_TARGETS_FILE"
rm -f "$TARGETS_FILE" "$INSTALLED_TARGETS_FILE" "$ABSENT_TARGETS_FILE" 2>/dev/null
[ "$FAILURES" -eq "$_stage_failures" ] && stage_done OK accounted || stage_done WARN applog-partial

stage verify "Memverifikasi identitas canonical dan overlay"
if [ -n "$COMMITTED_IDENTITY_SHA256" ] &&
   run_capture "$BIN" verify --run-id "$RUN_ID" \
       --identity-sha256 "$COMMITTED_IDENTITY_SHA256"; then
    stage_done OK verified
else
    failure "Verifikasi akhir gagal; identitas baru dipertahankan untuk forward recovery."
    stage_done FAIL verify-failed
fi

if [ "$FAILURES" -gt 0 ]; then
    finish 20 partial forward-recovery
fi
if [ "$REBOOT" -eq 1 ]; then
    finish 10 success reboot-required
fi
if [ "$WARNINGS" -gt 0 ]; then
    finish 0 success complete-with-warnings
fi
finish 0 success complete
