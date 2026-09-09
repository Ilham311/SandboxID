#!/system/bin/sh

MODDIR="${MODDIR:-/data/adb/modules/sandboxid}"
LOGFILE="${LOGFILE:-/cache/sandboxid-boot.log}"
IDENTITY_FILE="${IDENTITY_FILE:-$MODDIR/identity.prop}"
CARRIER_CONF="${CARRIER_CONF:-$MODDIR/carrier.conf}"
CARRIERS_FILE="${CARRIERS_FILE:-$MODDIR/carriers.tsv}"
BACKUP_DIR_ROOT="${BACKUP_DIR_ROOT:-$MODDIR/backups}"

mkdir -p "$BACKUP_DIR_ROOT" 2>/dev/null
chmod 0700 "$BACKUP_DIR_ROOT" 2>/dev/null

[ -w "$(dirname "$LOGFILE")" ] || LOGFILE="$MODDIR/sandboxid-boot.log"
touch "$LOGFILE" 2>/dev/null

ACTION_LOCK="${ACTION_LOCK:-$MODDIR/.action.lock}"
MUTATION_LOCK="${MUTATION_LOCK:-$MODDIR/.mutation.lock}"
MUTATION_LOCK_HELD=0
MUTATION_OWNER_PID=""
MUTATION_OWNER_START=""
MUTATION_OWNER_KIND=""
MUTATION_OWNER_TOKEN=""

_lock_fields() {
    _lf_file="$1"
    shift
    awk -F= -v requested="$(printf '%s\n' "$@")" '
        BEGIN {
            count = split(requested, order, "\n")
            for (i = 1; i <= count; ++i) wanted[order[i]] = 1
        }
        ($1 in wanted) && !seen[$1]++ {
            key = $1
            sub(/^[^=]*=/, "")
            values[key] = $0
        }
        END {
            for (i = 1; i <= count; ++i) print values[order[i]]
        }
    ' "$_lf_file" 2>/dev/null
}

_proc_stat_field() {
    _ps_pid="$1"; _ps_field="$2"
    [ -r "/proc/$_ps_pid/stat" ] || return 1
    awk -v n="$_ps_field" '{ line=$0; sub(/^.*\) /, "", line); split(line, f, " "); print f[n]; exit }' \
        "/proc/$_ps_pid/stat" 2>/dev/null
}

proc_start_ticks() { _proc_stat_field "$1" 20; }
proc_parent_pid() { _proc_stat_field "$1" 2; }

_process_descends_from() {
    _pd_child="$1"; _pd_owner="$2"; _pd_depth=0
    case "$_pd_child:$_pd_owner" in *[!0-9:]*) return 1 ;; esac
    while [ "$_pd_depth" -lt 64 ] && [ "$_pd_child" -gt 0 ]; do
        [ "$_pd_child" = "$_pd_owner" ] && return 0
        _pd_next="$(proc_parent_pid "$_pd_child" 2>/dev/null)"
        case "$_pd_next" in ''|*[!0-9]*) return 1 ;; esac
        [ "$_pd_next" != "$_pd_child" ] || return 1
        _pd_child="$_pd_next"
        _pd_depth=$((_pd_depth + 1))
    done
    return 1
}

_owner_binding_live() {
    _ob_pid="$1"
    _ob_start="$2"
    case "$_ob_pid:$_ob_start" in *[!0-9:]*|:*|*:) return 1 ;; esac
    kill -0 "$_ob_pid" 2>/dev/null || return 1
    [ "$(proc_start_ticks "$_ob_pid" 2>/dev/null)" = "$_ob_start" ]
}

_owner_file_live() {
    _of_file="$1"
    [ -r "$_of_file" ] || return 1
    _of_fields="$(_lock_fields "$_of_file" pid proc_start)" || return 1
    if ! {
        IFS= read -r _of_pid
        IFS= read -r _of_start
    } <<EOF
$_of_fields
EOF
    then
        return 1
    fi
    _owner_binding_live "$_of_pid" "$_of_start"
}

_action_lock_owned() {
    [ -n "${SBX_ACTION_RUN_ID:-}" ] || return 1
    [ -n "${SBX_MUTATION_OWNER_PID:-}" ] || return 1
    [ -n "${SBX_MUTATION_OWNER_START:-}" ] || return 1
    [ -n "${SBX_MUTATION_OWNER_TOKEN:-}" ] || return 1
    [ -r "$ACTION_LOCK/owner" ] || return 1
    _al_fields="$(_lock_fields "$ACTION_LOCK/owner" version kind run pid proc_start token)" || return 1
    if ! {
        IFS= read -r _al_version
        IFS= read -r _al_kind
        IFS= read -r _al_run
        IFS= read -r _al_pid
        IFS= read -r _al_start
        IFS= read -r _al_token
    } <<EOF
$_al_fields
EOF
    then
        return 1
    fi
    [ "$_al_version" = 1 ] && [ "$_al_kind" = action ] || return 1
    [ "$_al_run" = "$SBX_ACTION_RUN_ID" ] || return 1
    [ "$_al_pid" = "$SBX_MUTATION_OWNER_PID" ] || return 1
    [ "$_al_start" = "$SBX_MUTATION_OWNER_START" ] || return 1
    [ "$_al_token" = "$SBX_MUTATION_OWNER_TOKEN" ] || return 1
    _owner_file_live "$ACTION_LOCK/owner" || return 1
    _process_descends_from "$$" "$_al_pid"
}

_action_owner_live() { _owner_file_live "$ACTION_LOCK/owner"; }

_mutation_inherited_owned() {
    [ -n "${SBX_MUTATION_OWNER_PID:-}" ] || return 1
    [ -n "${SBX_MUTATION_OWNER_START:-}" ] || return 1
    [ -n "${SBX_MUTATION_OWNER_TOKEN:-}" ] || return 1
    [ -r "$MUTATION_LOCK/owner" ] || return 1
    _mi_fields="$(_lock_fields "$MUTATION_LOCK/owner" version pid proc_start kind run token)" || return 1
    if ! {
        IFS= read -r _mi_version
        IFS= read -r _mi_pid
        IFS= read -r _mi_start
        IFS= read -r _mi_kind
        IFS= read -r _mi_run
        IFS= read -r _mi_token
    } <<EOF
$_mi_fields
EOF
    then
        return 1
    fi
    [ "$_mi_version" = 1 ] || return 1
    [ "$_mi_pid" = "$SBX_MUTATION_OWNER_PID" ] || return 1
    [ "$_mi_start" = "$SBX_MUTATION_OWNER_START" ] || return 1
    [ "$_mi_token" = "$SBX_MUTATION_OWNER_TOKEN" ] || return 1
    _owner_file_live "$MUTATION_LOCK/owner" || return 1
    _process_descends_from "$$" "$_mi_pid" || return 1
    case "$_mi_kind" in
        action)
            [ "$_mi_run" = "${SBX_ACTION_RUN_ID:-}" ] || return 1
            _action_lock_owned ;;
        standalone)
            [ -z "$_mi_run" ] || return 1
            [ "$MUTATION_LOCK_HELD" -eq 1 ] || return 1
            [ "$MUTATION_OWNER_KIND" = standalone ] || return 1
            [ "$MUTATION_OWNER_PID" = "$_mi_pid" ] || return 1
            [ "$MUTATION_OWNER_START" = "$_mi_start" ] || return 1
            [ "$MUTATION_OWNER_TOKEN" = "$_mi_token" ] ;;
        *) return 1 ;;
    esac
}

action_mutation_allowed() {
    if [ -d "$MUTATION_LOCK" ]; then
        _mutation_inherited_owned
        return $?
    fi
    [ ! -d "$ACTION_LOCK" ] && return 0
    _action_lock_owned && return 0
    [ -r "$ACTION_LOCK/owner" ] || return 1
    _action_owner_live && return 1
    return 0
}

require_action_mutation() {
    action_mutation_allowed && return 0
    log_warn "mutasi ditolak: domain mutasi dimiliki operasi lain atau token owner tidak valid"
    return 1
}

_make_owner_token() {
    _mo_token="$(od -An -N16 -tx1 /dev/urandom 2>/dev/null | tr -d ' \r\n')"
    case "$_mo_token" in
        [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]) printf '%s\n' "$_mo_token" ;;
        *) return 1 ;;
    esac
}

mutation_lock_acquire() {
    _ml_kind="${1:-standalone}"
    _ml_run="${2:-}"
    [ "$MUTATION_LOCK_HELD" -eq 0 ] || return 0
    if _mutation_inherited_owned; then
        return 0
    fi
    if mkdir "$MUTATION_LOCK" 2>/dev/null; then
        :
    else
        [ -r "$MUTATION_LOCK/owner" ] || return 75
        _owner_file_live "$MUTATION_LOCK/owner" && return 75
        _ml_stale="${MUTATION_LOCK}.stale.$$"
        mv "$MUTATION_LOCK" "$_ml_stale" 2>/dev/null || return 75
        rm -rf "$_ml_stale" 2>/dev/null || return 1
        mkdir "$MUTATION_LOCK" 2>/dev/null || return 75
    fi
    chmod 0700 "$MUTATION_LOCK" 2>/dev/null || {
        rmdir "$MUTATION_LOCK" 2>/dev/null
        return 1
    }
    _ml_start="$(proc_start_ticks "$$" 2>/dev/null)"
    _ml_token="$(_make_owner_token 2>/dev/null)"
    case "$_ml_start:$_ml_token" in *[!0-9a-f:]*) rm -rf "$MUTATION_LOCK" 2>/dev/null; return 1 ;; esac
    case "$_ml_kind" in action) [ -n "$_ml_run" ] || { rm -rf "$MUTATION_LOCK" 2>/dev/null; return 1; } ;; standalone) _ml_run="" ;; *) rm -rf "$MUTATION_LOCK" 2>/dev/null; return 1 ;; esac
    _ml_tmp="$MUTATION_LOCK/owner.tmp.$$"
    umask 077
    if ! {
        printf 'version=1\n'
        printf 'kind=%s\n' "$_ml_kind"
        printf 'pid=%s\n' "$$"
        printf 'proc_start=%s\n' "$_ml_start"
        [ -n "$_ml_run" ] && printf 'run=%s\n' "$_ml_run"
        printf 'token=%s\n' "$_ml_token"
    } > "$_ml_tmp" 2>/dev/null ||
       ! chmod 0600 "$_ml_tmp" 2>/dev/null ||
       ! mv -f "$_ml_tmp" "$MUTATION_LOCK/owner" 2>/dev/null; then
        rm -f "$_ml_tmp" 2>/dev/null
        rm -rf "$MUTATION_LOCK" 2>/dev/null
        umask 022
        return 1
    fi
    umask 022
    MUTATION_OWNER_PID=$$
    MUTATION_OWNER_START=$_ml_start
    MUTATION_OWNER_KIND=$_ml_kind
    MUTATION_OWNER_TOKEN=$_ml_token
    MUTATION_LOCK_HELD=1
    SBX_MUTATION_OWNER_PID=$MUTATION_OWNER_PID
    SBX_MUTATION_OWNER_START=$MUTATION_OWNER_START
    SBX_MUTATION_OWNER_TOKEN=$MUTATION_OWNER_TOKEN
    export SBX_MUTATION_OWNER_PID SBX_MUTATION_OWNER_START SBX_MUTATION_OWNER_TOKEN
    return 0
}

mutation_lock_release() {
    [ "$MUTATION_LOCK_HELD" -eq 1 ] || return 0
    _mr_fields="$(_lock_fields "$MUTATION_LOCK/owner" pid proc_start token)" || _mr_fields=""
    {
        IFS= read -r _mr_pid
        IFS= read -r _mr_start
        IFS= read -r _mr_token
    } <<EOF
$_mr_fields
EOF
    if [ "$_mr_pid" = "$MUTATION_OWNER_PID" ] &&
       [ "$_mr_start" = "$MUTATION_OWNER_START" ] &&
       [ "$_mr_token" = "$MUTATION_OWNER_TOKEN" ]; then
        rm -f "$MUTATION_LOCK/owner" 2>/dev/null
        rmdir "$MUTATION_LOCK" 2>/dev/null
    fi
    MUTATION_LOCK_HELD=0
    MUTATION_OWNER_PID=""
    MUTATION_OWNER_START=""
    MUTATION_OWNER_KIND=""
    MUTATION_OWNER_TOKEN=""
    unset SBX_MUTATION_OWNER_PID SBX_MUTATION_OWNER_START SBX_MUTATION_OWNER_TOKEN
}

_now() { date '+%Y-%m-%d %H:%M:%S'; }
_log() { printf '[%s] %s\n' "$(_now)" "$*" | tee -a "$LOGFILE"; }
log_step() { _log "==> $*"; }
log_info() { _log "    $*"; }
log_ok()   { _log "[OK] $*"; }
log_warn() { _log "[WARN] $*"; }
log_err()  { _log "[ERR] $*"; }

mask_id() {
    _v="$1"
    [ -z "$_v" ] && { printf '(empty)'; return; }
    _len=${#_v}
    if [ "$_len" -le 6 ]; then
        printf '******'
    else
        printf '%s****%s' "$(printf '%s' "$_v" | cut -c1-4)" "$(printf '%s' "$_v" | cut -c$((_len-1))-)"
    fi
}

_SE_REF=0
_SE_PRIOR=""
se_restore_all() {
    while [ "$_SE_REF" -gt 0 ]; do
        se_restore
    done
}
se_permissive() {
    if [ "$_SE_REF" -eq 0 ]; then
        _SE_PRIOR="$(getenforce 2>/dev/null || echo Unknown)"
        setenforce 0 2>/dev/null || true
    fi
    _SE_REF=$((_SE_REF + 1))
}
se_restore() {
    [ "$_SE_REF" -gt 0 ] && _SE_REF=$((_SE_REF - 1))
    if [ "$_SE_REF" -eq 0 ]; then
        _se_restore_rc=0
        if [ "$_SE_PRIOR" = "Enforcing" ]; then
            setenforce 1 2>/dev/null || _se_restore_rc=1
        fi
        _SE_PRIOR=""
        if [ "$_se_restore_rc" -ne 0 ]; then
            log_warn "SELinux enforcing tidak dapat dipulihkan"
            return 1
        fi
    fi
    return 0
}

get_users() {
    if [ -d /data/system/users ]; then
        for d in /data/system/users/[0-9]*; do
            [ -d "$d" ] || continue
            basename "$d"
        done
    else
        echo 0
    fi
}

generate_uuid() {
    if [ -r /proc/sys/kernel/random/uuid ]; then
        cat /proc/sys/kernel/random/uuid
        return
    fi
    r=$(od -An -N16 -tx1 /dev/urandom 2>/dev/null | tr -d ' \n' | cut -c1-32)
    printf '%s-%s-4%s-%s-%s\n' \
        "$(echo "$r" | cut -c1-8)" \
        "$(echo "$r" | cut -c9-12)" \
        "$(echo "$r" | cut -c14-16)" \
        "$(echo "$r" | cut -c17-20)" \
        "$(echo "$r" | cut -c21-32)"
}

generate_mac() {
    b=$(od -An -N5 -tx1 /dev/urandom 2>/dev/null | tr -d ' \n')
    printf '02:%s:%s:%s:%s:%s\n' \
        "$(echo "$b" | cut -c1-2)" \
        "$(echo "$b" | cut -c3-4)" \
        "$(echo "$b" | cut -c5-6)" \
        "$(echo "$b" | cut -c7-8)" \
        "$(echo "$b" | cut -c9-10)"
}

_fw_run() {
    _n=0
    while [ "$_n" -lt 2 ]; do
        "$@" </dev/null >/dev/null 2>&1 && return 0
        _n=$((_n + 1))
        [ "$_n" -lt 2 ] && sleep 1
    done
    return 1
}

settings_put() {
    scope="$1"; key="$2"; val="$3"
    command -v settings >/dev/null 2>&1 || return 1
    _fw_run settings put --user 0 "$scope" "$key" "$val"
}

rp_set() {
    key="$1"; val="$2"
    case "$key" in
        persist.*) _rpflag="-p" ;;
        *)         _rpflag="-n" ;;
    esac
    if command -v resetprop >/dev/null 2>&1; then
        resetprop "$_rpflag" "$key" "$val" 2>/dev/null && return 0
    fi
    if [ -x "$MODDIR/bin/resetprop-rs" ]; then
        "$MODDIR/bin/resetprop-rs" "$_rpflag" "$key" "$val" 2>/dev/null && return 0
    fi
    if command -v resetprop-rs >/dev/null 2>&1; then
        resetprop-rs "$_rpflag" "$key" "$val" 2>/dev/null && return 0
    fi
    setprop "$key" "$val" 2>/dev/null
}

sbx_bin() {
    _d="${MODDIR:-/data/adb/modules/sandboxid}/bin"
    if [ -x "$_d/sandboxid" ]; then
        printf '%s\n' "$_d/sandboxid"
        return 0
    fi
    _abi="$(getprop ro.product.cpu.abi 2>/dev/null)"
    case "$_abi" in
        arm64-v8a)   _abi=arm64 ;;
        armeabi-v7a) _abi=arm ;;
        x86_64)      _abi=x86_64 ;;
        x86)         _abi=x86 ;;
        *)
            case "$(uname -m 2>/dev/null)" in
                aarch64)       _abi=arm64 ;;
                armv7l|armv8l) _abi=arm ;;
                x86_64)        _abi=x86_64 ;;
                i686|i386)     _abi=x86 ;;
                *)             _abi="" ;;
            esac
            ;;
    esac
    [ -n "$_abi" ] || return 1
    for _n in "sandboxid-$_abi" "ternak-tt-$_abi"; do
        if [ -x "$_d/$_n" ]; then
            printf '%s\n' "$_d/$_n"
            return 0
        fi
    done
    return 1
}

force_stop() {
    pkg="$1"
    command -v am >/dev/null 2>&1 || return 1
    _fw_run am force-stop --user 0 "$pkg"
    _rc=$?
    command -v killall >/dev/null 2>&1 && killall "$pkg" 2>/dev/null
    return "$_rc"
}

identity_get() {
    key="$1"
    [ -f "$IDENTITY_FILE" ] || return 1
    awk -F= -v k="$key" '$1==k { sub(/^[^=]*=/, ""); print; exit }' "$IDENTITY_FILE" 2>/dev/null
}

identity_persist() {
    key="$1"; val="$2"
    [ -z "$key" ] && return 1
    require_action_mutation || return 75
    _cli="$(sbx_bin 2>/dev/null)"
    [ -x "$_cli" ] || {
        log_warn "identity update ditolak: binary native tidak tersedia"
        return 1
    }
    case "$key" in
        GOOGLE_AID|WIFI_MAC|BLUETOOTH_ADDR|BLUETOOTH_NAME|BOOT_COUNT)
            "$_cli" set-local "$key" "$val" </dev/null >/dev/null 2>&1 ;;
        SBX_NATIVE_READ|SBX_HIDE|SBX_CPU_REVISION|SBX_PROC_VERSION|SBX_MEMINFO|SBX_SYSFS_MAC)
            "$_cli" set-flag "$key" "$val" </dev/null >/dev/null 2>&1 ;;
        *)
            log_warn "identity update '$key' tidak memiliki transaksi native"
            return 64 ;;
    esac
}

identity_del() {
    log_warn "identity_del dinonaktifkan; gunakan perintah native yang memiliki transaksi"
    return 64
}

identity_preserve_operational_flags() {
    log_warn "flag preservation shell dinonaktifkan; native prepare mempertahankan flag"
    return 64
}

backup_rotate() {
    prefix="$1"; keep="${2:-10}"
    [ -d "$BACKUP_DIR_ROOT" ] || return 0
    ls -1t "$BACKUP_DIR_ROOT"/${prefix}* 2>/dev/null | tail -n +"$((keep + 1))" | while read -r f; do
        rm -f "$f" 2>/dev/null
    done
}

_valid_package_name() {
    _vp_name="$1"
    case "$_vp_name" in
        ''|.*|*.|*..*|*:*|*/*|*[!A-Za-z0-9._]*) return 1 ;;
    esac
    case "$_vp_name" in
        *.*) : ;;
        *) return 1 ;;
    esac
    _vp_oldifs=$IFS
    IFS=.
    set -- $_vp_name
    IFS=$_vp_oldifs
    [ "$#" -ge 2 ] || return 1
    for _vp_part do
        case "$_vp_part" in
            ''|[0-9]*|*[!A-Za-z0-9_]*) return 1 ;;
        esac
    done
}

_applog_path_manifest() {
    printf '%s\n' \
        shared_prefs/applog.xml shared_prefs/applog.xml.bak \
        shared_prefs/applog_stats.xml shared_prefs/applog_stats.xml.bak \
        shared_prefs/applog_last_sp_session.xml shared_prefs/applog_last_sp_session.xml.bak \
        shared_prefs/applog_last_data.xml shared_prefs/applog_last_data.xml.bak \
        shared_prefs/applog_pack.xml shared_prefs/applog_pack.xml.bak \
        shared_prefs/applog_easter_egg.xml shared_prefs/applog_easter_egg.xml.bak \
        shared_prefs/snssdk_openudid.xml shared_prefs/snssdk_openudid.xml.bak \
        shared_prefs/snssdk_did.xml shared_prefs/snssdk_did.xml.bak \
        shared_prefs/bd_device_info.xml shared_prefs/bd_device_info.xml.bak \
        shared_prefs/header_custom.xml shared_prefs/header_custom.xml.bak \
        shared_prefs/ug_install_settings_pref.xml shared_prefs/ug_install_settings_pref.xml.bak \
        files/bd_setting/device_id files/bd_setting/openudid \
        files/bd_setting/clientudid files/bd_setting/install_id \
        files/.cdid files/applog files/applog_v2 files/applog_v3 \
        files/bd_tracker_n no_backup/applog_device_id.dat \
        no_backup/bd_device_id no_backup/.cdid
}

applog_wipe() {
    require_action_mutation || return 75
    _pkg="${1:-}"
    if [ -z "$_pkg" ]; then
        _cli="$(sbx_bin 2>/dev/null)"
        [ -x "$_cli" ] || {
            log_warn "applog_wipe: binary native tidak tersedia"
            return 1
        }
        _targets="${TMPDIR:-/data/local/tmp}/sbx-targets.$$"
        if ! "$_cli" targets --packages > "$_targets" 2>/dev/null; then
            rm -f "$_targets" 2>/dev/null
            log_warn "applog_wipe: daftar target tidak valid"
            return 1
        fi
        _rc=0
        while IFS= read -r _line || [ -n "$_line" ]; do
            [ -n "$_line" ] || continue
            applog_wipe "$_line" || _rc=1
        done < "$_targets"
        rm -f "$_targets" 2>/dev/null
        return "$_rc"
    fi

    if ! _valid_package_name "$_pkg"; then
        log_warn "applog_wipe: nama package tidak valid"
        return 64
    fi

    _data_dir=""
    for _base in /data/data /data/user/0; do
        [ -d "$_base/$_pkg" ] && { _data_dir="$_base/$_pkg"; break; }
    done
    if [ -z "$_data_dir" ]; then
        log_info "applog_wipe: $_pkg not installed — skipped"
        return 1
    fi

    log_step "AppLog wipe: $_pkg ($_data_dir)"

    if ! force_stop "$_pkg" >/dev/null 2>&1; then
        log_warn "applog_wipe: force-stop gagal untuk $_pkg; cache tidak disentuh"
        return 1
    fi

    se_permissive
    _wipe_failed=0
    _sp_dir="$_data_dir/shared_prefs"
    _ts=$(date +%s)
    _safe_pkg=$(printf '%s' "$_pkg" | tr '/. ' '___')
    _bkp="$BACKUP_DIR_ROOT/applog_${_safe_pkg}_${_ts}.tar"
    _backup_list="${TMPDIR:-/data/local/tmp}/sbx-applog-backup.$$"
    : > "$_backup_list" 2>/dev/null || {
        se_restore
        log_warn "applog_wipe: daftar backup gagal dibuat untuk $_pkg; cache tidak disentuh"
        return 1
    }
    _manifest="${TMPDIR:-/data/local/tmp}/sbx-applog-manifest.$$"
    _applog_path_manifest > "$_manifest" 2>/dev/null || {
        rm -f "$_backup_list" "$_manifest" 2>/dev/null
        se_restore
        log_warn "applog_wipe: manifest cache gagal dibuat untuk $_pkg"
        return 1
    }
    while IFS= read -r _candidate || [ -n "$_candidate" ]; do
        [ -e "$_data_dir/$_candidate" ] && printf '%s\n' "$_candidate" >> "$_backup_list"
    done < "$_manifest"
    if [ -s "$_backup_list" ]; then
        if ! ( cd "$_data_dir" && tar -cf "$_bkp" -T "$_backup_list" 2>/dev/null ) ||
           [ ! -s "$_bkp" ] || ! chmod 0600 "$_bkp" 2>/dev/null; then
            rm -f "$_bkp" "$_backup_list" "$_manifest" 2>/dev/null
            se_restore
            log_warn "applog_wipe: backup gagal untuk $_pkg; cache tidak disentuh"
            return 1
        fi
    fi
    rm -f "$_backup_list" 2>/dev/null

    _removed=0
    while IFS= read -r _candidate || [ -n "$_candidate" ]; do
        _abs="$_data_dir/$_candidate"
        case "$_abs" in
            "$_data_dir"/shared_prefs/*|"$_data_dir"/files/*|"$_data_dir"/no_backup/*) : ;;
            *) _wipe_failed=$((_wipe_failed + 1)); continue ;;
        esac
        [ -e "$_abs" ] || continue
        if rm -rf "${_abs:?}" 2>/dev/null && [ ! -e "$_abs" ]; then
            _removed=$((_removed + 1))
        else
            _wipe_failed=$((_wipe_failed + 1))
        fi
    done < "$_manifest"
    rm -f "$_manifest" 2>/dev/null

    if ! se_restore; then
        _wipe_failed=$((_wipe_failed + 1))
    fi
    backup_rotate "applog_" 20

    if [ "$_wipe_failed" -gt 0 ]; then
        log_warn "$_pkg — AppLog wipe parsial: $_removed terhapus, $_wipe_failed gagal"
        return 1
    fi
    if [ "$_removed" -gt 0 ]; then
        log_ok "$_pkg — cleared $_removed AppLog cache entr(y|ies)"
    else
        log_info "$_pkg — no AppLog cache present (already clean)"
    fi
    return 0
}

_applog_own() {
    _t="$1"; _uid="$2"; _mode="$3"; _refctx="$4"
    [ -e "$_t" ] || return 1
    [ -n "$_uid" ] || return 1
    chown "${_uid}:${_uid}" "$_t" 2>/dev/null || return 1
    chmod "$_mode" "$_t" 2>/dev/null || return 1
    if [ -n "$_refctx" ]; then
        command -v chcon >/dev/null 2>&1 || return 1
        chcon "$_refctx" "$_t" 2>/dev/null || return 1
    fi
    return 0
}

_applog_map() {
    printf "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<map>\n"
    while [ "$#" -ge 2 ]; do
        [ -n "$2" ] && printf '    <string name="%s">%s</string>\n' "$1" "$2"
        shift 2
    done
    printf '</map>\n'
}

_applog_put() {
    _dst="$1"; _mode="$2"; _uid="$3"; _refctx="$4"; _body="$5"
    _tmp="${_dst%/*}/.sbxseed.$$"
    if ! printf '%s\n' "$_body" > "$_tmp" 2>/dev/null || [ ! -s "$_tmp" ]; then
        rm -f "$_tmp" 2>/dev/null
        return 1
    fi
    if ! mv -f "$_tmp" "$_dst" 2>/dev/null; then
        rm -f "$_tmp" 2>/dev/null
        return 1
    fi
    _applog_own "$_dst" "$_uid" "$_mode" "$_refctx" && return 0
    rm -f "$_dst" 2>/dev/null
    return 1
}

applog_seed() {
    require_action_mutation || return 75
    _pkg="${1:-}"
    [ -n "$_pkg" ] || return 1
    _valid_package_name "$_pkg" || return 64

    _data_dir=""
    for _base in /data/data /data/user/0; do
        [ -d "$_base/$_pkg" ] && { _data_dir="$_base/$_pkg"; break; }
    done
    [ -n "$_data_dir" ] || return 1

    _cli="$(sbx_bin 2>/dev/null)"
    if [ -z "$_cli" ] || [ ! -x "$_cli" ]; then
        log_warn "applog_seed: binary native tidak ada; seed tidak dilakukan"
        return 1
    fi
    _ids="$("$_cli" applog-ids "$_pkg" 2>/dev/null)"
    if [ -z "$_ids" ]; then
        log_warn "applog_seed: applog-ids gagal untuk $_pkg"
        return 1
    fi
    _did=$(printf '%s\n' "$_ids"        | awk -F= '$1=="DID"{print $2;exit}')
    _iid=$(printf '%s\n' "$_ids"        | awk -F= '$1=="IID"{print $2;exit}')
    _ssid=$(printf '%s\n' "$_ids"       | awk -F= '$1=="SSID"{print $2;exit}')
    _openudid=$(printf '%s\n' "$_ids"   | awk -F= '$1=="OPENUDID"{print $2;exit}')
    _clientudid=$(printf '%s\n' "$_ids" | awk -F= '$1=="CLIENTUDID"{print $2;exit}')
    _cdid=$(printf '%s\n' "$_ids"       | awk -F= '$1=="CDID"{print $2;exit}')
    if [ -z "$_did" ] || [ -z "$_iid" ] || [ -z "$_ssid" ] || [ -z "$_cdid" ] || \
       [ -z "$_openudid" ] || [ -z "$_clientudid" ]; then
        log_warn "applog_seed: output applog-ids tidak lengkap untuk $_pkg"
        return 1
    fi

    _uid=$(stat -c '%u' "$_data_dir" 2>/dev/null)
    case "$_uid" in
        ''|*[!0-9]*)
            log_warn "applog_seed: uid $_pkg tidak terbaca (stat '$_data_dir') — seed dibatalkan, file root-owned tidak ditinggalkan"
            return 1 ;;
    esac
    _sp="$_data_dir/shared_prefs"
    _bd="$_data_dir/files/bd_setting"

    se_permissive
    if ! mkdir -p "$_sp" "$_bd" 2>/dev/null ||
       [ ! -d "$_sp" ] || [ ! -d "$_data_dir/files" ] || [ ! -d "$_bd" ]; then
        se_restore
        log_warn "applog_seed: direktori seed gagal dibuat untuk $_pkg"
        return 1
    fi
    _spctx=$(ls -Zd "$_data_dir" 2>/dev/null | awk '{print $1}')
    case "$_spctx" in
        u:object_r:*) : ;;
        *)
            se_restore
            log_warn "applog_seed: konteks SELinux $_pkg tidak dapat diverifikasi; seed dibatalkan"
            return 1 ;;
    esac
    command -v chcon >/dev/null 2>&1 || {
        se_restore
        log_warn "applog_seed: chcon tidak tersedia; seed dibatalkan"
        return 1
    }
    for _d in "$_sp" "$_data_dir/files" "$_bd"; do
        [ -d "$_d" ] || continue
        if ! _applog_own "$_d" "$_uid" 0771 "$_spctx"; then
            se_restore
            log_warn "applog_seed: gagal chown $_d ke uid $_uid — seed dibatalkan"
            return 1
        fi
    done

    _seeded=0
    _failed=0
    _applog_put "$_sp/applog.xml" 0660 "$_uid" "$_spctx" \
        "$(_applog_map device_id "$_did" install_id "$_iid" ssid "$_ssid" cdid "$_cdid")" \
        && _seeded=$((_seeded + 1)) || _failed=$((_failed + 1))
    _applog_put "$_sp/snssdk_openudid.xml" 0660 "$_uid" "$_spctx" \
        "$(_applog_map openudid "$_openudid" clientudid "$_clientudid")" \
        && _seeded=$((_seeded + 1)) || _failed=$((_failed + 1))

    for _pair in "device_id=$_did" "install_id=$_iid" "openudid=$_openudid" \
                 "clientudid=$_clientudid" ".cdid=$_cdid"; do
        _n=${_pair%%=*}; _v=${_pair#*=}
        [ -n "$_v" ] || continue
        case "$_n" in
            .cdid) _dst="$_data_dir/files/.cdid" ;;
            *)     _dst="$_bd/$_n" ;;
        esac
        if _applog_put "$_dst" 0600 "$_uid" "$_spctx" "$_v"; then
            _seeded=$((_seeded + 1))
        else
            _failed=$((_failed + 1))
        fi
    done
    se_restore

    if [ "$_failed" -gt 0 ]; then
        log_warn "$_pkg — seed parsial: $_seeded ok, $_failed gagal (izin/SELinux)"
        return 1
    fi
    if [ "$_seeded" -gt 0 ]; then
        log_ok "$_pkg — seeded $_seeded file (did=$(mask_id "$_did"))"
        return 0
    fi
    log_warn "$_pkg — seed gagal total (izin/SELinux?)"
    return 1
}

applog_regen() {
    require_action_mutation || return 75
    _pkg="${1:-}"
    if [ -z "$_pkg" ]; then
        _cli="$(sbx_bin 2>/dev/null)"
        [ -x "$_cli" ] || return 1
        _targets="${TMPDIR:-/data/local/tmp}/sbx-targets.$$"
        if ! "$_cli" targets --packages > "$_targets" 2>/dev/null; then
            rm -f "$_targets" 2>/dev/null
            return 1
        fi
        _rc=0
        while IFS= read -r _line || [ -n "$_line" ]; do
            [ -n "$_line" ] || continue
            applog_regen "$_line" || _rc=1
        done < "$_targets"
        rm -f "$_targets" 2>/dev/null
        return "$_rc"
    fi

    if ! _valid_package_name "$_pkg"; then
        log_warn "applog_regen: nama package tidak valid"
        return 64
    fi

    _found=0
    for _base in /data/data /data/user/0; do
        [ -d "$_base/$_pkg" ] && { _found=1; break; }
    done
    if [ "$_found" = 0 ]; then
        log_info "applog_regen: $_pkg not installed — skipped"
        return 1
    fi

    log_step "AppLog regen: $_pkg"

    # APPLOG_EPOCH belongs to the committed snapshot.  Never bump it per
    # package: every target in one Action must derive IDs from the same epoch.
    force_stop "$_pkg" >/dev/null 2>&1
    if ! applog_wipe "$_pkg"; then
        log_warn "$_pkg — cache AppLog tidak dapat dibersihkan"
        return 1
    fi
    if applog_seed "$_pkg"; then
        log_ok "$_pkg — cache wiped + seed dari epoch canonical"
        return 0
    fi
    log_warn "$_pkg — cache wiped, tetapi seed disk gagal"
    return 1
}

applog_probe() {
    _pkg="${1:-}"
    [ -n "$_pkg" ] || return 1
    _data_dir=""
    for _base in /data/data /data/user/0; do
        [ -d "$_base/$_pkg" ] && { _data_dir="$_base/$_pkg"; break; }
    done
    if [ -z "$_data_dir" ]; then
        printf '%s 0 absent\n' "$_pkg"
        return 0
    fi
    _sp="$_data_dir/shared_prefs"
    _bd="$_data_dir/files/bd_setting"
    _count=0
    for _f in \
        "$_sp/applog.xml" "$_sp/applog_stats.xml" "$_sp/snssdk_openudid.xml" \
        "$_sp/bd_device_info.xml" "$_bd/device_id" "$_bd/install_id" \
        "$_bd/openudid" "$_bd/clientudid" "$_data_dir/files/.cdid"
    do
        [ -e "$_f" ] && _count=$((_count + 1))
    done
    _state=fresh
    if [ -f "$_sp/applog.xml" ] || [ -f "$_bd/device_id" ]; then
        _state=active
    fi
    printf '%s %d %s\n' "$_pkg" "$_count" "$_state"
    return 0
}
