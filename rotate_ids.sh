#!/system/bin/sh

set -u
MODDIR="${MODDIR:-/data/adb/modules/sandboxid}"
LOGFILE="${LOGFILE:-/cache/sandboxid-boot.log}"

if [ -r "$MODDIR/helpers.sh" ]; then
    . "$MODDIR/helpers.sh"
else
    echo "[ERR] $MODDIR/helpers.sh not found" >&2
    exit 2
fi

REBOOT_NEEDED=0
FAILURES=0
UNSUPPORTED=0
FROM_IDENTITY=0
REPORT_FILE=""
COMPONENT_REPORT=""
MUTATION_LOCK_OWNED=0

rotation_cleanup() {
    command -v se_restore_all >/dev/null 2>&1 && se_restore_all
    if [ "$MUTATION_LOCK_OWNED" -eq 1 ]; then
        mutation_lock_release
        MUTATION_LOCK_OWNED=0
    fi
}

rotation_signal() {
    _signal_rc="$1"
    rotation_cleanup
    trap - EXIT INT TERM HUP
    exit "$_signal_rc"
}

trap rotation_cleanup EXIT
trap 'rotation_signal 129' HUP
trap 'rotation_signal 130' INT
trap 'rotation_signal 143' TERM

component_status() {
    _component="$1"
    _status="$2"
    _rc="${3:-0}"
    COMPONENT_REPORT="${COMPONENT_REPORT}${_component}=${_status}:${_rc}
"
}

record_report() {
    [ -n "$REPORT_FILE" ] || return 0
    _tmp="${REPORT_FILE}.tmp.$$"
    {
        printf 'version=1\n'
        printf 'run=%s\n' "${SBX_ACTION_RUN_ID:-standalone}"
        printf 'REBOOT_NEEDED=%s\n' "$REBOOT_NEEDED"
        printf 'FAILURES=%s\n' "$FAILURES"
        printf 'UNSUPPORTED=%s\n' "$UNSUPPORTED"
        printf '%s' "$COMPONENT_REPORT"
    } > "$_tmp" 2>/dev/null || return 1
    chmod 0644 "$_tmp" 2>/dev/null || { rm -f "$_tmp"; return 1; }
    command -v sync >/dev/null 2>&1 && sync "$_tmp" 2>/dev/null || :
    mv -f "$_tmp" "$REPORT_FILE" 2>/dev/null || return 1
    command -v sync >/dev/null 2>&1 && sync -d "${REPORT_FILE%/*}" 2>/dev/null || :
    return 0
}

acquire_rotation_lock() {
    if [ "$FROM_IDENTITY" -eq 1 ]; then
        require_action_mutation || return 75
        return 0
    fi
    mutation_lock_acquire standalone
    _rc=$?
    if [ "$_rc" -eq 0 ]; then
        MUTATION_LOCK_OWNED=1
        return 0
    fi
    [ "$_rc" -eq 75 ] && log_warn "mutasi ditolak: operasi rotasi lain sedang aktif"
    return "$_rc"
}

valid_uuid_v4_or_zero() {
    case "$1" in
        00000000-0000-0000-0000-000000000000) return 0 ;;
        ????????-????-4???-[89aAbB]???-????????????) : ;;
        *) return 1 ;;
    esac
    case "$1" in *[!0-9a-fA-F-]*) return 1 ;; esac
    return 0
}

require_snapshot_value() {
    _key="$1"
    _value="$(identity_get "$_key" 2>/dev/null)"
    [ -n "$_value" ] || {
        log_err "snapshot canonical tidak memiliki $_key"
        return 1
    }
    printf '%s\n' "$_value"
}

require_standalone_local() {
    _key="$1"
    _value="$2"
    [ "$FROM_IDENTITY" -eq 1 ] && return 0
    require_action_mutation || return 75
    _cli="$(sbx_bin 2>/dev/null)"
    [ -x "$_cli" ] || {
        log_err "set-local $_key: binary native tidak tersedia"
        return 127
    }
    "$_cli" set-local "$_key" "$_value" </dev/null >/dev/null 2>&1 || {
        _rc=$?
        log_err "set-local $_key ditolak (rc=$_rc); store eksternal tidak dimutasi"
        return "$_rc"
    }
}

fresh_local_value() {
    _key="$1"
    case "$_key" in
        GOOGLE_AID) generate_uuid ;;
        BOOT_COUNT)
            _old="$(identity_get BOOT_COUNT 2>/dev/null || true)"
            case "$_old" in ''|*[!0-9]*) _old=0 ;; esac
            printf '%s\n' "$((_old + 1))" ;;
        *) return 1 ;;
    esac
}

run_step() {
    _component="$1"
    shift
    "$@"
    _rc=$?
    case "$_rc" in
        0)
            component_status "$_component" ok 0
            return 0 ;;
        3)
            UNSUPPORTED=$((UNSUPPORTED + 1))
            component_status "$_component" unsupported 3
            return 3 ;;
        *)
            FAILURES=$((FAILURES + 1))
            component_status "$_component" failed "$_rc"
            return "$_rc" ;;
    esac
}

wipe_ssaid() {
    log_step "Wipe SSAID (backup + verified removal)"
    se_permissive
    changed=0
    failed=0
    _stamp="$(date +%s).$$"
    for u in $(get_users); do
        f="/data/system/users/$u/settings_ssaid.xml"
        [ -f "$f" ] || continue
        backup="$BACKUP_DIR_ROOT/settings_ssaid.$u.$_stamp.bak"
        if ! cp -f "$f" "$backup" 2>/dev/null || [ ! -s "$backup" ]; then
            log_warn "SSAID user $u: backup gagal; file tidak dihapus"
            failed=$((failed + 1))
            continue
        fi
        if rm -f "$f" "$f.bak" "$f.tmp" 2>/dev/null &&
           [ ! -e "$f" ] && [ ! -e "$f.bak" ] && [ ! -e "$f.tmp" ]; then
            changed=1
        else
            log_warn "SSAID user $u: penghapusan tidak terverifikasi"
            failed=$((failed + 1))
        fi
    done
    se_restore
    backup_rotate "settings_ssaid." 10
    if [ "$changed" = "1" ]; then
        REBOOT_NEEDED=1
        log_ok "SSAID cleared (backup in $BACKUP_DIR_ROOT)"
        log_warn "REBOOT REQUIRED: system_server regenerates SSAID at boot."
    else
        log_info "No settings_ssaid.xml removed"
    fi
    [ "$failed" -eq 0 ]
}

set_gaid_value() {
    newgaid="${1:-}"
    [ -z "$newgaid" ] && newgaid="$(fresh_local_value GOOGLE_AID)"
    if ! valid_uuid_v4_or_zero "$newgaid"; then
        log_err "GAID harus UUID-v4 atau sentinel nol"
        return 2
    fi
    require_standalone_local GOOGLE_AID "$newgaid" || return $?
    case "$newgaid" in
        00000000-0000-0000-0000-000000000000) gaid_opt_out=1 ;;
        *) gaid_opt_out=0 ;;
    esac
    log_step "Set local GAID storage: $(mask_id "$newgaid")"

    gaid_failed=0
    settings_put global advertising_id "$newgaid" || {
        log_warn "settings put advertising_id failed"
        gaid_failed=1
    }
    settings_put global limit_ad_tracking "$gaid_opt_out" || {
        log_warn "settings put limit_ad_tracking failed"
        gaid_failed=1
    }

    force_stop com.google.android.gms >/dev/null 2>&1 || :
    command -v am >/dev/null 2>&1 && am kill --user 0 com.google.android.gms </dev/null >/dev/null 2>&1
    sleep 1

    se_permissive
    GMS_DIR="${SBX_GMS_DIR:-/data/data/com.google.android.gms}"
    if [ ! -d "$GMS_DIR" ]; then
        se_restore
        log_info "GMS not installed — local Settings state only"
        [ "$gaid_failed" -eq 0 ]
        return $?
    fi

    ADID="$GMS_DIR/shared_prefs/adid_settings.xml"
    gaid_had_primary=0
    if [ -f "$ADID" ]; then
        gaid_had_primary=1
        backup="$BACKUP_DIR_ROOT/adid_settings.$(date +%s).$$.xml"
        cp -f "$ADID" "$backup" 2>/dev/null && [ -s "$backup" ] || {
            se_restore
            log_warn "GAID cache backup gagal; file lama dipertahankan"
            return 1
        }
    fi
    rm -f "$ADID" "$GMS_DIR"/shared_prefs/adsidentity*.xml \
        "$GMS_DIR"/files/adid_cache.dat 2>/dev/null || gaid_failed=1
    rm -rf "$GMS_DIR"/no_backup/adid* 2>/dev/null || gaid_failed=1
    mkdir -p "$GMS_DIR/shared_prefs" 2>/dev/null || gaid_failed=1
    tmp_adid="${ADID}.tmp.$$"
    {
        printf "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n"
        printf '<map>\n'
        printf '    <string name="adid_key">%s</string>\n' "$newgaid"
        printf '    <boolean name="enable_limit_ad_tracking" value="%s" />\n' \
            "$([ "$gaid_opt_out" = "1" ] && printf true || printf false)"
        printf '    <long name="last_reset_time" value="%s000" />\n' "$(date +%s)"
        printf '</map>\n'
    } > "$tmp_adid" 2>/dev/null || gaid_failed=1
    gms_uid=$(stat -c '%u' "$GMS_DIR" 2>/dev/null)
    case "$gms_uid" in ''|*[!0-9]*) gaid_failed=1 ;; esac
    [ "$gaid_failed" -eq 0 ] && chown "${gms_uid}:${gms_uid}" "$tmp_adid" 2>/dev/null || gaid_failed=1
    [ "$gaid_failed" -eq 0 ] && chmod 0660 "$tmp_adid" 2>/dev/null || gaid_failed=1
    if [ "$gaid_failed" -eq 0 ] && command -v chcon >/dev/null 2>&1; then
        parent_ctx=$(ls -Zd "$GMS_DIR/shared_prefs" 2>/dev/null | awk '{print $1}')
        case "$parent_ctx" in u:object_r:*) chcon "$parent_ctx" "$tmp_adid" 2>/dev/null || gaid_failed=1 ;; esac
    fi
    if [ "$gaid_failed" -eq 0 ]; then
        mv -f "$tmp_adid" "$ADID" 2>/dev/null || gaid_failed=1
    fi
    if [ "$gaid_failed" -ne 0 ]; then
        if [ "$gaid_had_primary" -eq 1 ]; then
            if ! cp -f "$backup" "$ADID" 2>/dev/null || [ ! -s "$ADID" ]; then
                log_warn "GAID cache lama tidak dapat dipulihkan"
            fi
        else
            rm -f "$ADID" 2>/dev/null
        fi
    fi
    rm -f "$tmp_adid" 2>/dev/null
    se_restore
    backup_rotate "adid_settings." 10
    if [ "$gaid_failed" -ne 0 ]; then
        log_warn "GAID local storage tidak selesai koheren"
        return 1
    fi
    log_ok "GAID written: $(mask_id "$newgaid")"
    return 0
}

regen_applog() {
    _arg="${1:-}"
    log_step "Regenerate ByteDance AppLog IDs${_arg:+ ($_arg)}"

    if [ -n "$_arg" ]; then
        applog_regen "$_arg"
        return $?
    fi

    if ! grep -qE '^[[:space:]]*[^[:space:]#]' "$MODDIR/target.txt" 2>/dev/null; then
        log_info "target.txt kosong — tidak ada aplikasi ByteDance yang diregenerasi"
        log_info "Hint: 'rotate_ids.sh applog <package>' atau isi target.txt dulu"
        return 0
    fi
    applog_regen
}

wipe_applog_only() {
    _arg="${1:-}"
    log_step "Wipe-only (no seed) ByteDance AppLog cache${_arg:+ ($_arg)}"
    if [ -n "$_arg" ]; then
        applog_wipe "$_arg"
        return $?
    fi
    if ! grep -qE '^[[:space:]]*[^[:space:]#]' "$MODDIR/target.txt" 2>/dev/null; then
        log_info "target.txt kosong — tidak ada yang di-wipe"
        return 0
    fi
    applog_wipe
}

sync_boot_count() {
    bc="${1:-}"
    [ -z "$bc" ] && bc="$(fresh_local_value BOOT_COUNT)"
    case "$bc" in
        ''|*[!0-9]*)
            log_err "boot_count snapshot tidak valid"
            return 2 ;;
    esac
    require_standalone_local BOOT_COUNT "$bc" || return $?
    log_step "Set Settings.Global.boot_count = $bc (dari snapshot canonical)"
    if settings_put global boot_count "$bc"; then
        log_ok "boot_count ke $bc"
        return 0
    fi
    log_warn "settings put boot_count gagal (mungkin tidak diizinkan perangkat)"
    return 1
}

cmd_status() {
    log_step "Current identifier state"
    if [ -f "$IDENTITY_FILE" ]; then
        log_info "identity.prop  : $IDENTITY_FILE"
        for k in MODEL DEVICE BRAND SERIAL ANDROID_ID GOOGLE_AID BOOT_COUNT; do
            v="$(identity_get "$k" 2>/dev/null || true)"
            [ -z "$v" ] && continue
            case "$k" in
                SERIAL|ANDROID_ID|GOOGLE_AID) v="$(mask_id "$v")" ;;
            esac
            log_info "  $k = $v"
        done
    else
        log_info "identity.prop  : missing (run 'sandboxid freshen')"
    fi
    if command -v settings >/dev/null 2>&1; then
        log_info "Settings.Global.advertising_id  = $(mask_id "$(settings get --user 0 global advertising_id </dev/null 2>/dev/null)")"
        log_info "Settings.Global.device_name     = $(settings get --user 0 global device_name </dev/null 2>/dev/null)"
    fi
    log_info "getprop ro.product.model              = $(getprop ro.product.model 2>/dev/null)"
    log_info "getprop ro.serialno                   = $(mask_id "$(getprop ro.serialno 2>/dev/null)")"
    for u in $(get_users); do
        f="/data/system/users/$u/settings_ssaid.xml"
        if [ -f "$f" ]; then
            log_info "  user $u SSAID xml = present ($(stat -c '%s' "$f") bytes)"
        else
            log_info "  user $u SSAID xml = absent"
        fi
    done

    if grep -qE '^[[:space:]]*[^[:space:]#]' "$MODDIR/target.txt" 2>/dev/null; then
        log_info "AppLog cache (per target):"
        _cli="$(sbx_bin 2>/dev/null)"
        _targets="${TMPDIR:-/data/local/tmp}/sbx-status-targets.$$"
        if [ -x "$_cli" ] && "$_cli" targets --packages > "$_targets" 2>/dev/null; then
            while IFS= read -r _t || [ -n "$_t" ]; do
                [ -n "$_t" ] || continue
                _probe=$(applog_probe "$_t" 2>/dev/null)
                if [ -n "$_probe" ]; then
                    _n=$(printf '%s' "$_probe" | awk '{print $2}')
                    _st=$(printf '%s' "$_probe" | awk '{print $3}')
                    log_info "  $_t = $_n file(s), state=$_st"
                else
                    log_info "  $_t = (probe unavailable)"
                fi
            done < "$_targets"
        else
            log_info "  (daftar target tidak valid atau binary native tidak tersedia)"
        fi
        rm -f "$_targets" 2>/dev/null
    fi
}

cmd="${1:-all}"
shift 2>/dev/null || true
MODVER=$(awk -F= '$1=="version"{print $2}' "$MODDIR/module.prop" 2>/dev/null)
log_step "rotate_ids.sh cmd=$cmd (module $MODVER)"

if [ "$cmd" = all ] && [ "${1:-}" = --from-identity ]; then
    FROM_IDENTITY=1
fi
case "$cmd" in
    status|-h|--help|help) : ;;
    *) acquire_rotation_lock || exit $? ;;
esac

if [ "$cmd" = all ] && [ "${1:-}" = --from-identity ]; then
    shift
    [ "$#" -eq 0 ] || {
        log_err "all --from-identity tidak menerima argumen tambahan"
        exit 64
    }
    [ -n "${SBX_ACTION_RUN_ID:-}" ] || {
        log_err "all --from-identity hanya valid di dalam Action"
        exit 75
    }
    require_action_mutation || exit 75
    FROM_IDENTITY=1
    REPORT_FILE="$MODDIR/debug/rotation.$SBX_ACTION_RUN_ID"
    rm -f "$REPORT_FILE" 2>/dev/null
    GAID_VALUE="$(require_snapshot_value GOOGLE_AID)" || FAILURES=$((FAILURES + 1))
    BOOT_VALUE="$(require_snapshot_value BOOT_COUNT)" || FAILURES=$((FAILURES + 1))
    APPLOG_VALUE="$(require_snapshot_value APPLOG_EPOCH)" || FAILURES=$((FAILURES + 1))
    valid_uuid_v4_or_zero "${GAID_VALUE:-}" || FAILURES=$((FAILURES + 1))
    case "${BOOT_VALUE:-}" in ''|*[!0-9]*) FAILURES=$((FAILURES + 1)) ;; esac
    case "${APPLOG_VALUE:-}" in ''|*[!0-9]*) FAILURES=$((FAILURES + 1)) ;; esac
    if [ "$FAILURES" -eq 0 ]; then
        run_step ssaid wipe_ssaid || :
        run_step gaid set_gaid_value "$GAID_VALUE" || :
        run_step boot_count sync_boot_count "$BOOT_VALUE" || :
    else
        component_status snapshot failed 2
        log_err "snapshot rotasi tidak lengkap/valid; tidak ada store yang dimutasi"
    fi
    if ! record_report; then
        FAILURES=$((FAILURES + 1))
        log_err "laporan rotasi tidak dapat dipersistenkan"
    fi
else
    case "$cmd" in
        all)
            run_step ssaid wipe_ssaid || :
            run_step gaid set_gaid_value "$@" || :
            run_step boot_count sync_boot_count || :
            run_step applog regen_applog || :
            ;;
        safe)
            run_step gaid set_gaid_value "$@" || :
            run_step boot_count sync_boot_count || :
            run_step applog regen_applog || :
            ;;
        ssaid)                run_step ssaid wipe_ssaid || : ;;
        gaid)                 run_step gaid set_gaid_value "$@" || : ;;
        boot-count|bootcount) run_step boot_count sync_boot_count "$@" || : ;;
        applog|bytedance|regen-applog) run_step applog regen_applog "$@" || : ;;
        applog-wipe|wipe-applog)       run_step applog_wipe wipe_applog_only "$@" || : ;;
        status)               cmd_status ;;
        -h|--help|help)
            cat <<USAGE
Usage: rotate_ids.sh <cmd> [args]
  all [--from-identity]       - full rotation; Action mode consumes canonical values only
  safe                       - local GAID + boot count + AppLog
  ssaid                      - wipe settings_ssaid.xml after verified backup (needs reboot)
  gaid [uuid]                - write validated local Advertising ID caches
  boot-count [count]         - write Settings.Global.boot_count
  applog [pkg]               - wipe + seed AppLog using canonical APPLOG_EPOCH
  applog-wipe [pkg]          - wipe AppLog cache only
  status                     - show current values (read-only)
USAGE
            exit 0 ;;
        *) log_err "Unknown cmd: $cmd (try: rotate_ids.sh help)"; exit 64 ;;
    esac
fi

[ "$REBOOT_NEEDED" = "1" ] && log_warn "REBOOT REQUIRED for SSAID regeneration."
if [ "$FAILURES" -gt 0 ]; then
    log_warn "rotate_ids.sh: $FAILURES step(s) reported failure"
    exit 1
fi
if [ "$UNSUPPORTED" -gt 0 ]; then
    log_warn "rotate_ids.sh: $UNSUPPORTED component(s) unsupported"
    exit 3
fi
log_ok "rotate_ids.sh $cmd completed cleanly"
exit 0
