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

valid_local_mac() {
    case "$1" in
        [0-9a-fA-F][0-9a-fA-F]:[0-9a-fA-F][0-9a-fA-F]:[0-9a-fA-F][0-9a-fA-F]:[0-9a-fA-F][0-9a-fA-F]:[0-9a-fA-F][0-9a-fA-F]:[0-9a-fA-F][0-9a-fA-F]) : ;;
        *) return 1 ;;
    esac
    _octet="$(printf '%s' "$1" | cut -d: -f1)"
    _value=$((0x$_octet))
    [ $((_value & 1)) -eq 0 ] && [ $((_value & 2)) -eq 2 ] &&
        [ "$1" != "00:00:00:00:00:00" ]
}

valid_device_name() {
    [ -n "$1" ] && [ "${#1}" -le 64 ] || return 1
    case "$1" in ' '*|*' '|*'&'*|*'<'*|*'>'*|*'"'*|*"'"*) return 1 ;; esac
    _clean="$(printf '%s' "$1" | LC_ALL=C tr -d '\040-\176')"
    [ -z "$_clean" ]
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
        WIFI_MAC|BLUETOOTH_ADDR) generate_mac ;;
        BLUETOOTH_NAME) printf 'Sandbox-%s\n' "$(od -An -N3 -tx1 /dev/urandom 2>/dev/null | tr -d ' \r\n')" ;;
        BOOT_COUNT)
            _old="$(identity_get BOOT_COUNT 2>/dev/null || true)"
            case "$_old" in ''|*[!0-9]*) _old=0 ;; esac
            printf '%s\n' "$((_old + 1))" ;;
        *) return 1 ;;
    esac
}

_carrier_lookup_id() {
    _lk_mcc="$1"; _lk_mnc="$2"
    [ -r "$CARRIERS_FILE" ] || return 0
    awk -F'\t' -v mcc="$_lk_mcc" -v mnc="$_lk_mnc" '
        /^[[:space:]]*#/ || NF < 4 { next }
        $2 == mcc && $3 == mnc { gsub(/[[:space:]]/, "", $5); print $5; exit }
    ' "$CARRIERS_FILE" 2>/dev/null
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

randomize_wlan_mac() {
    newmac="${1:-}"
    [ -z "$newmac" ] && newmac="$(fresh_local_value WIFI_MAC)"
    if ! valid_local_mac "$newmac"; then
        log_err "wlan MAC harus unicast locally administered"
        return 2
    fi
    require_standalone_local WIFI_MAC "$newmac" || return $?
    log_step "Apply wlan0 MAC from snapshot: $(mask_id "$newmac")"

    if ! command -v ip >/dev/null 2>&1; then
        log_warn "ip(8) not available; wlan MAC unsupported"
        return 3
    fi
    if ! ip link show wlan0 </dev/null >/dev/null 2>&1; then
        log_warn "interface wlan0 tidak tersedia"
        return 3
    fi
    se_permissive
    wlan_failed=0
    ip link set wlan0 down 2>/dev/null || wlan_failed=1
    sleep 1
    ip link set dev wlan0 address "$newmac" 2>/dev/null || wlan_failed=1
    ip link set wlan0 up 2>/dev/null || wlan_failed=1

    WCS=/data/misc/apexdata/com.android.wifi/WifiConfigStore.xml
    if [ -f "$WCS" ]; then
        backup="$BACKUP_DIR_ROOT/WifiConfigStore.$(date +%s).$$.xml"
        if cp -f "$WCS" "$backup" 2>/dev/null && [ -s "$backup" ]; then
            rm -f "$WCS" "$WCS.encrypted-checkpoint" 2>/dev/null || wlan_failed=1
        else
            log_warn "WifiConfigStore backup gagal; cache tidak dihapus"
            wlan_failed=1
        fi
    fi
    se_restore
    backup_rotate "WifiConfigStore." 5
    if [ "$wlan_failed" -ne 0 ]; then
        log_warn "wlan0 MAC/cache apply parsial"
        return 1
    fi
    log_ok "MAC: $(mask_id "$newmac")"
    return 0
}

rotate_bluetooth_mac() {
    newbt="${1:-}"
    [ -z "$newbt" ] && newbt="$(fresh_local_value BLUETOOTH_ADDR)"
    if ! valid_local_mac "$newbt"; then
        log_err "Bluetooth MAC harus unicast locally administered"
        return 2
    fi
    require_standalone_local BLUETOOTH_ADDR "$newbt" || return $?
    log_step "Apply Bluetooth adapter MAC from snapshot: $(mask_id "$newbt")"

    bt_failed=0
    for _prop in persist.service.bdroid.bdaddr persist.sys.bt.bdaddr \
                 persist.bluetooth.bdaddr bluetooth.device.mac.address \
                 ro.boot.btmacaddr; do
        rp_set "$_prop" "$newbt" || bt_failed=1
    done

    se_permissive
    updated=0
    found=0
    _bt_index=0
    for btcfg in /data/misc/bluedroid/bt_config.conf \
                 /data/misc/bluetooth/bt_config.conf \
                 /data/vendor/bluetooth/bt_config.conf; do
        [ -f "$btcfg" ] || continue
        _bt_index=$((_bt_index + 1))
        found=1
        owner=$(stat -c '%u:%g' "$btcfg" 2>/dev/null)
        mode=$(stat -c '%a' "$btcfg" 2>/dev/null)
        ctx=$(ls -Zd "$btcfg" 2>/dev/null | awk '{print $1}')
        backup="$BACKUP_DIR_ROOT/bt_config_addr.$(date +%s).$$.$_bt_index.conf"
        if ! cp -f "$btcfg" "$backup" 2>/dev/null || [ ! -s "$backup" ]; then
            log_warn "Bluetooth config backup gagal: $btcfg"
            bt_failed=1
            continue
        fi
        if grep -q '^Address = ' "$btcfg" 2>/dev/null; then
            awk -v m="$newbt" '/^Address = / { print "Address = " m; next } { print }' \
                "$btcfg" > "${btcfg}.tmp" 2>/dev/null
        else
            awk -v m="$newbt" 'BEGIN{d=0} /^\[Adapter\]/ && !d { print; print "Address = " m; d=1; next } { print } END { if (!d) exit 1 }' \
                "$btcfg" > "${btcfg}.tmp" 2>/dev/null
        fi
        if [ -s "${btcfg}.tmp" ] && mv -f "${btcfg}.tmp" "$btcfg" 2>/dev/null &&
           [ -n "$owner" ] && chown "$owner" "$btcfg" 2>/dev/null &&
           [ -n "$mode" ] && chmod "$mode" "$btcfg" 2>/dev/null; then
            case "$ctx" in u:object_r:*) chcon "$ctx" "$btcfg" 2>/dev/null || bt_failed=1 ;; esac
            updated=$((updated + 1))
        else
            bt_failed=1
        fi
        rm -f "${btcfg}.tmp" 2>/dev/null
    done
    se_restore
    backup_rotate "bt_config_addr." 10
    if [ "$found" -eq 0 ]; then
        log_info "No bt_config.conf present; property backend only"
    elif [ "$updated" -eq 0 ]; then
        bt_failed=1
    fi

    force_stop com.android.bluetooth >/dev/null 2>&1 || :
    pkill -f 'com\.(android|google\.android)\.bluetooth' 2>/dev/null || :
    sleep 1
    if [ "$bt_failed" -ne 0 ]; then
        log_warn "Bluetooth MAC apply parsial"
        return 1
    fi
    log_ok "BT MAC applied: $(mask_id "$newbt") (toggle BT off/on to activate)"
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

sync_device_name() {
    NEW_NAME="${1:-}"
    [ -z "$NEW_NAME" ] && NEW_NAME="$(fresh_local_value BLUETOOTH_NAME)"
    if ! valid_device_name "$NEW_NAME"; then
        log_err "device-name snapshot kosong atau tidak valid"
        return 2
    fi
    require_standalone_local BLUETOOTH_NAME "$NEW_NAME" || return $?
    log_step "Sync device/BT name -> $NEW_NAME (from canonical snapshot)"
    name_failed=0
    settings_put global bluetooth_name "$NEW_NAME" || name_failed=1
    settings_put global device_name "$NEW_NAME" || name_failed=1
    rp_set persist.bluetooth.adaptername "$NEW_NAME" || name_failed=1

    se_permissive
    updated=0
    _bt_index=0
    for btcfg in /data/misc/bluedroid/bt_config.conf \
                 /data/misc/bluetooth/bt_config.conf \
                 /data/vendor/bluetooth/bt_config.conf; do
        [ -f "$btcfg" ] || continue
        grep -q '^Name = ' "$btcfg" 2>/dev/null || continue
        _bt_index=$((_bt_index + 1))
        owner=$(stat -c '%u:%g' "$btcfg" 2>/dev/null)
        mode=$(stat -c '%a' "$btcfg" 2>/dev/null)
        ctx=$(ls -Zd "$btcfg" 2>/dev/null | awk '{print $1}')
        backup="$BACKUP_DIR_ROOT/bt_config_name.$(date +%s).$$.$_bt_index.conf"
        if ! cp -f "$btcfg" "$backup" 2>/dev/null || [ ! -s "$backup" ]; then
            name_failed=1
            continue
        fi
        awk -v n="$NEW_NAME" '/^Name = / { print "Name = " n; next } { print }' \
            "$btcfg" > "${btcfg}.tmp" 2>/dev/null
        if [ -s "${btcfg}.tmp" ] && mv -f "${btcfg}.tmp" "$btcfg" 2>/dev/null &&
           [ -n "$owner" ] && chown "$owner" "$btcfg" 2>/dev/null &&
           [ -n "$mode" ] && chmod "$mode" "$btcfg" 2>/dev/null; then
            case "$ctx" in u:object_r:*) chcon "$ctx" "$btcfg" 2>/dev/null || name_failed=1 ;; esac
            updated=$((updated + 1))
        else
            name_failed=1
        fi
        rm -f "${btcfg}.tmp" 2>/dev/null
    done
    se_restore
    backup_rotate "bt_config_name." 10

    force_stop com.android.bluetooth >/dev/null 2>&1 || :
    pkill -f 'com\.(android|google\.android)\.bluetooth' 2>/dev/null || :
    sleep 1
    if [ "$name_failed" -ne 0 ]; then
        log_warn "device/BT name apply parsial"
        return 1
    fi
    log_ok "Name: $NEW_NAME"
    return 0
}

set_carrier() {
    spec="${1:-status}"
    _cli="$(sbx_bin 2>/dev/null)"
    [ -x "$_cli" ] || {
        log_err "carrier: binary native tidak tersedia"
        return 127
    }
    case "$spec" in
        status|'')
            log_step "Status operator / kartu SIM"
            if [ -f "$CARRIER_CONF" ]; then
                cn="$(awk -F= '$1=="NAME"{sub(/^[^=]*=/,"");print;exit}' "$CARRIER_CONF" 2>/dev/null)"
                cm="$(awk -F= '$1=="MCC"{print $2;exit}' "$CARRIER_CONF" 2>/dev/null)"
                cc="$(awk -F= '$1=="MNC"{print $2;exit}' "$CARRIER_CONF" 2>/dev/null)"
                log_info "Operator : ${cn:-(kosong)}"
                log_info "Kode     : ${cm}${cc}"
            else
                log_info "Operator kustom nonaktif"
            fi
            log_info "identity GSM_OPERATOR_NUMERIC = $(identity_get GSM_OPERATOR_NUMERIC 2>/dev/null || true)"
            log_info "identity GSM_OPERATOR_ALPHA   = $(identity_get GSM_OPERATOR_ALPHA 2>/dev/null || true)"
            return 0 ;;
        off|none|clear|default)
            require_action_mutation || return 75
            log_step "Nonaktifkan operator kustom"
            "$_cli" carrier disable </dev/null || return $?
            log_ok "Operator kustom dinonaktifkan secara atomik"
            return 0 ;;
    esac

    mcc="$(printf '%s' "$spec" | awk -F'|' '{print $1}' | tr -d ' \t\r')"
    mnc="$(printf '%s' "$spec" | awk -F'|' '{print $2}' | tr -d ' \t\r')"
    name="$(printf '%s' "$spec" | awk -F'|' '{print $3}' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
    iso="$(printf '%s' "$spec" | awk -F'|' '{print $4}' | tr -d ' \t\r' | tr '[:upper:]' '[:lower:]')"
    phantom="$(printf '%s' "$spec" | awk -F'|' '{print $5}' | tr -d ' \t\r')"
    [ "$phantom" = "1" ] || phantom=0
    cid="$(printf '%s' "$spec" | awk -F'|' '{print $6}' | tr -d ' \t\r')"
    [ -z "$cid" ] && cid="$(_carrier_lookup_id "$mcc" "$mnc" | tr -d ' \t\r')"
    case "$cid" in -1) cid="" ;; esac

    require_action_mutation || return 75
    tmpc="${TMPDIR:-/data/local/tmp}/sbx-carrier.$$"
    umask 077
    {
        printf 'NAME=%s\n' "$name"
        printf 'MCC=%s\n' "$mcc"
        printf 'MNC=%s\n' "$mnc"
        printf 'ISO=%s\n' "$iso"
        printf 'PHANTOM=%s\n' "$phantom"
        printf 'CARRIER_ID=%s\n' "$cid"
    } > "$tmpc" 2>/dev/null || {
        rm -f "$tmpc" 2>/dev/null
        umask 022
        return 1
    }
    umask 022
    if "$_cli" carrier apply "$tmpc" </dev/null; then
        rm -f "$tmpc" 2>/dev/null
        log_ok "Operator aktif: $name ($mcc$mnc)"
        return 0
    fi
    _rc=$?
    rm -f "$tmpc" 2>/dev/null
    return "$_rc"
}

cmd_status() {
    log_step "Current identifier state"
    if [ -f "$IDENTITY_FILE" ]; then
        log_info "identity.prop  : $IDENTITY_FILE"
        for k in MODEL DEVICE BRAND SERIAL ANDROID_ID GOOGLE_AID WIFI_MAC BLUETOOTH_ADDR BLUETOOTH_NAME; do
            v="$(identity_get "$k" 2>/dev/null || true)"
            [ -z "$v" ] && continue
            case "$k" in
                SERIAL|ANDROID_ID|GOOGLE_AID|WIFI_MAC|BLUETOOTH_ADDR) v="$(mask_id "$v")" ;;
            esac
            log_info "  $k = $v"
        done
    else
        log_info "identity.prop  : missing (run 'sandboxid freshen')"
    fi
    if command -v settings >/dev/null 2>&1; then
        log_info "Settings.Global.advertising_id  = $(mask_id "$(settings get --user 0 global advertising_id </dev/null 2>/dev/null)")"
        log_info "Settings.Global.device_name     = $(settings get --user 0 global device_name </dev/null 2>/dev/null)"
        log_info "Settings.Global.bluetooth_name  = $(settings get --user 0 global bluetooth_name </dev/null 2>/dev/null)"
    fi
    log_info "getprop ro.product.model              = $(getprop ro.product.model 2>/dev/null)"
    log_info "getprop persist.bluetooth.adaptername = $(getprop persist.bluetooth.adaptername 2>/dev/null)"
    log_info "getprop persist.service.bdroid.bdaddr = $(mask_id "$(getprop persist.service.bdroid.bdaddr 2>/dev/null)")"
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
    WIFI_VALUE="$(require_snapshot_value WIFI_MAC)" || FAILURES=$((FAILURES + 1))
    BT_VALUE="$(require_snapshot_value BLUETOOTH_ADDR)" || FAILURES=$((FAILURES + 1))
    NAME_VALUE="$(require_snapshot_value BLUETOOTH_NAME)" || FAILURES=$((FAILURES + 1))
    BOOT_VALUE="$(require_snapshot_value BOOT_COUNT)" || FAILURES=$((FAILURES + 1))
    APPLOG_VALUE="$(require_snapshot_value APPLOG_EPOCH)" || FAILURES=$((FAILURES + 1))
    valid_uuid_v4_or_zero "${GAID_VALUE:-}" || FAILURES=$((FAILURES + 1))
    valid_local_mac "${WIFI_VALUE:-}" || FAILURES=$((FAILURES + 1))
    valid_local_mac "${BT_VALUE:-}" || FAILURES=$((FAILURES + 1))
    valid_device_name "${NAME_VALUE:-}" || FAILURES=$((FAILURES + 1))
    case "${BOOT_VALUE:-}" in ''|*[!0-9]*) FAILURES=$((FAILURES + 1)) ;; esac
    case "${APPLOG_VALUE:-}" in ''|*[!0-9]*) FAILURES=$((FAILURES + 1)) ;; esac
    if [ "$FAILURES" -eq 0 ]; then
        run_step ssaid wipe_ssaid || :
        run_step gaid set_gaid_value "$GAID_VALUE" || :
        run_step wlan_mac randomize_wlan_mac "$WIFI_VALUE" || :
        run_step bluetooth_mac rotate_bluetooth_mac "$BT_VALUE" || :
        run_step device_name sync_device_name "$NAME_VALUE" || :
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
            run_step wlan_mac randomize_wlan_mac || :
            run_step bluetooth_mac rotate_bluetooth_mac || :
            run_step device_name sync_device_name || :
            run_step boot_count sync_boot_count || :
            run_step applog regen_applog || :
            ;;
        safe)
            run_step gaid set_gaid_value "$@" || :
            run_step bluetooth_mac rotate_bluetooth_mac || :
            run_step device_name sync_device_name || :
            run_step boot_count sync_boot_count || :
            run_step applog regen_applog || :
            ;;
        ssaid)                run_step ssaid wipe_ssaid || : ;;
        gaid)                 run_step gaid set_gaid_value "$@" || : ;;
        wlan-mac|mac)         run_step wlan_mac randomize_wlan_mac "$@" || : ;;
        bt-mac|bluetooth-mac) run_step bluetooth_mac rotate_bluetooth_mac "$@" || : ;;
        device-name|name)     run_step device_name sync_device_name "$@" || : ;;
        boot-count|bootcount) run_step boot_count sync_boot_count "$@" || : ;;
        carrier|sim)          run_step carrier set_carrier "$@" || : ;;
        applog|bytedance|regen-applog) run_step applog regen_applog "$@" || : ;;
        applog-wipe|wipe-applog)       run_step applog_wipe wipe_applog_only "$@" || : ;;
        status)               cmd_status ;;
        -h|--help|help)
            cat <<USAGE
Usage: rotate_ids.sh <cmd> [args]
  all [--from-identity]       - full rotation; Action mode consumes canonical values only
  safe                       - local GAID + BT MAC + device name + boot count + AppLog
  ssaid                      - wipe settings_ssaid.xml after verified backup (needs reboot)
  gaid [uuid]                - write validated local Advertising ID caches
  wlan-mac [xx:xx:..]        - apply a locally administered unicast wlan0 MAC
  bt-mac [xx:xx:..]          - apply a locally administered unicast Bluetooth MAC
  device-name [name]         - sync validated device/BT name
  boot-count [count]         - write Settings.Global.boot_count
  carrier <spec>|off|status  - native atomic carrier transaction; spec=MCC|MNC|NAME|ISO|PHANTOM|ID
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
