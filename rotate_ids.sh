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

wipe_ssaid() {
    log_step "Wipe SSAID (backup + surgical)"
    se_permissive
    changed=0
    for u in $(get_users); do
        f="/data/system/users/$u/settings_ssaid.xml"
        [ -f "$f" ] || continue
        cp -f "$f" "$BACKUP_DIR_ROOT/settings_ssaid.$u.$(date +%s).bak" 2>/dev/null
        rm -f "$f" "$f.bak" "$f.tmp" 2>/dev/null
        changed=1
    done
    se_restore
    backup_rotate "settings_ssaid." 10
    if [ "$changed" = "1" ]; then
        REBOOT_NEEDED=1
        log_ok "SSAID cleared (backup in $BACKUP_DIR_ROOT)"
        log_warn "REBOOT REQUIRED: system_server regenerates SSAID at boot."
    else
        log_info "No settings_ssaid.xml present"
    fi
    return 0
}

set_gaid_value() {
    newgaid="${1:-}"
    [ -z "$newgaid" ] && newgaid="$(identity_get GOOGLE_AID 2>/dev/null || true)"
    if [ -z "$newgaid" ]; then
        newgaid="$(generate_uuid)"
        identity_persist GOOGLE_AID "$newgaid"
        log_info "GAID generated + persisted to identity.prop"
    fi
    log_step "Set GAID: $(mask_id "$newgaid")"

    settings_put global advertising_id "$newgaid" || log_warn "settings put advertising_id failed"
    settings_put global limit_ad_tracking 0       || :

    force_stop com.google.android.gms
    command -v am >/dev/null 2>&1 && am kill --user 0 com.google.android.gms </dev/null >/dev/null 2>&1
    sleep 1

    se_permissive
    GMS_DIR=/data/data/com.google.android.gms
    if [ ! -d "$GMS_DIR" ]; then
        se_restore
        log_warn "GMS not installed - value queued in Settings.Global only"
        return 0
    fi

    ADID="$GMS_DIR/shared_prefs/adid_settings.xml"
    [ -f "$ADID" ] && cp -f "$ADID" "$BACKUP_DIR_ROOT/adid_settings.$(date +%s).xml" 2>/dev/null
    rm -f "$ADID" 2>/dev/null
    rm -f "$GMS_DIR"/shared_prefs/adsidentity*.xml 2>/dev/null
    rm -f "$GMS_DIR"/files/adid_cache.dat 2>/dev/null
    rm -rf "$GMS_DIR"/no_backup/adid* 2>/dev/null

    mkdir -p "$GMS_DIR/shared_prefs" 2>/dev/null
    cat > "$ADID" <<XMLEOF
<?xml version='1.0' encoding='utf-8' standalone='yes' ?>
<map>
    <string name="adid_key">$newgaid</string>
    <boolean name="enable_limit_ad_tracking" value="false" />
    <long name="last_reset_time" value="$(date +%s)000" />
</map>
XMLEOF
    gms_uid=$(stat -c '%u' "$GMS_DIR" 2>/dev/null)
    [ -n "$gms_uid" ] && chown "${gms_uid}:${gms_uid}" "$ADID" 2>/dev/null
    chmod 0660 "$ADID" 2>/dev/null
    if command -v chcon >/dev/null 2>&1; then
        parent_ctx=$(ls -Zd "$GMS_DIR/shared_prefs" 2>/dev/null | awk '{print $1}')
        [ -n "$parent_ctx" ] && chcon "$parent_ctx" "$ADID" 2>/dev/null
    fi
    se_restore
    backup_rotate "adid_settings." 10
    log_ok "GAID written: $(mask_id "$newgaid")"
    return 0
}

randomize_wlan_mac() {
    newmac="${1:-}"
    [ -z "$newmac" ] && newmac="$(identity_get WIFI_MAC 2>/dev/null || true)"
    if [ -z "$newmac" ]; then
        newmac="$(generate_mac)"
        identity_persist WIFI_MAC "$newmac"
        log_info "wlan MAC generated + persisted to identity.prop"
    fi
    log_step "Randomize wlan0 MAC: $(mask_id "$newmac")"

    if ! command -v ip >/dev/null 2>&1; then
        log_warn "ip(8) not available; MAC only recorded in identity.prop"
        return 1
    fi
    se_permissive
    ip link set wlan0 down 2>/dev/null
    sleep 1
    if ip link set dev wlan0 address "$newmac" 2>/dev/null; then
        log_ok "MAC: $(mask_id "$newmac")"
    else
        log_warn "MAC rejected by driver (Android 10+ uses per-SSID MAC)"
    fi
    ip link set wlan0 up 2>/dev/null

    WCS=/data/misc/apexdata/com.android.wifi/WifiConfigStore.xml
    if [ -f "$WCS" ]; then
        cp -f "$WCS" "$BACKUP_DIR_ROOT/WifiConfigStore.$(date +%s).xml" 2>/dev/null
        log_info "Backup WifiConfigStore -> $BACKUP_DIR_ROOT"
        rm -f "$WCS" "$WCS.encrypted-checkpoint" 2>/dev/null
    fi
    se_restore
    backup_rotate "WifiConfigStore." 5
    return 0
}

rotate_bluetooth_mac() {
    newbt="${1:-}"
    [ -z "$newbt" ] && newbt="$(identity_get BLUETOOTH_ADDR 2>/dev/null || true)"
    if [ -z "$newbt" ]; then
        newbt="$(generate_mac)"
        identity_persist BLUETOOTH_ADDR "$newbt"
        log_info "BT MAC generated + persisted to identity.prop"
    fi
    log_step "Rotate Bluetooth adapter MAC: $(mask_id "$newbt")"

    rp_set persist.service.bdroid.bdaddr "$newbt"
    rp_set persist.sys.bt.bdaddr         "$newbt"
    rp_set persist.bluetooth.bdaddr      "$newbt"
    rp_set bluetooth.device.mac.address  "$newbt"
    rp_set ro.boot.btmacaddr             "$newbt"

    se_permissive
    updated=0
    for btcfg in /data/misc/bluedroid/bt_config.conf \
                 /data/misc/bluetooth/bt_config.conf \
                 /data/vendor/bluetooth/bt_config.conf; do
        [ -f "$btcfg" ] || continue
        owner=$(stat -c '%U:%G' "$btcfg" 2>/dev/null)
        mode=$(stat -c '%a' "$btcfg" 2>/dev/null)
        cp -f "$btcfg" "$BACKUP_DIR_ROOT/bt_config_addr.$(date +%s).conf" 2>/dev/null
        if grep -q '^Address = ' "$btcfg" 2>/dev/null; then
            awk -v m="$newbt" '/^Address = / { print "Address = " m; next } { print }' \
                "$btcfg" > "${btcfg}.tmp" 2>/dev/null
        else
            awk -v m="$newbt" 'BEGIN{d=0} /^\[Adapter\]/ && !d { print; print "Address = " m; d=1; next } { print }' \
                "$btcfg" > "${btcfg}.tmp" 2>/dev/null
        fi
        if [ -s "${btcfg}.tmp" ] && mv "${btcfg}.tmp" "$btcfg" 2>/dev/null; then
            [ -n "$owner" ] && chown "$owner" "$btcfg" 2>/dev/null
            [ -n "$mode" ]  && chmod "$mode"  "$btcfg" 2>/dev/null
            log_ok "Rewrote Address in $btcfg"
            updated=1
        fi
        rm -f "${btcfg}.tmp" 2>/dev/null
    done
    [ "$updated" -eq 0 ] && log_warn "No writable bt_config.conf for Address"
    se_restore
    backup_rotate "bt_config_addr." 10

    force_stop com.android.bluetooth
    pkill -f 'com\.(android|google\.android)\.bluetooth' 2>/dev/null
    sleep 1
    log_ok "BT MAC applied: $(mask_id "$newbt") (toggle BT off/on to activate)"
    return 0
}

sync_boot_count() {
    # Terapkan BOOT_COUNT dari identity.prop ke Settings.Global.boot_count. Apps
    # yang baca Settings.Global.BOOT_COUNT (indikator "device sering reboot?")
    # jadi lihat angka persona kita, bukan angka device asli. Ini yang bikin
    # "boot count" beneran kepakai (dulu cuma metadata yang nggak pernah ditulis).
    bc="$(identity_get BOOT_COUNT 2>/dev/null || true)"
    case "$bc" in
        ''|*[!0-9]*)
            log_info "boot_count: identity.prop belum punya angka valid — dilewati"
            return 0 ;;
    esac
    log_step "Set Settings.Global.boot_count = $bc (dari identity.prop)"
    if settings_put global boot_count "$bc"; then
        log_ok "boot_count ke $bc"
    else
        log_warn "settings put boot_count gagal (mungkin tidak diizinkan perangkat)"
        return 1
    fi
    return 0
}

sync_device_name() {
    NEW_NAME="${1:-}"
    [ -z "$NEW_NAME" ] && NEW_NAME="$(identity_get BLUETOOTH_NAME 2>/dev/null || true)"
    [ -z "$NEW_NAME" ] && NEW_NAME="$(identity_get MODEL 2>/dev/null || true)"
    if [ -z "$NEW_NAME" ]; then
        log_err "device-name: identity.prop missing MODEL/BLUETOOTH_NAME."
        log_info "Hint: 'bin/sandboxid freshen' runs first via action.sh."
        return 1
    fi
    log_step "Sync device/BT name -> $NEW_NAME (from identity.prop, persona-consistent)"
    identity_persist BLUETOOTH_NAME "$NEW_NAME"

    settings_put global bluetooth_name "$NEW_NAME" || log_warn "settings put bluetooth_name failed"
    settings_put global device_name    "$NEW_NAME" || log_warn "settings put device_name failed"
    rp_set persist.bluetooth.adaptername "$NEW_NAME"

    se_permissive
    updated=0
    for btcfg in /data/misc/bluedroid/bt_config.conf \
                 /data/misc/bluetooth/bt_config.conf \
                 /data/vendor/bluetooth/bt_config.conf; do
        [ -f "$btcfg" ] || continue
        grep -q '^Name = ' "$btcfg" 2>/dev/null || continue
        owner=$(stat -c '%U:%G' "$btcfg" 2>/dev/null)
        mode=$(stat -c '%a' "$btcfg" 2>/dev/null)
        cp -f "$btcfg" "$BACKUP_DIR_ROOT/bt_config_name.$(date +%s).conf" 2>/dev/null
        awk -v n="$NEW_NAME" '/^Name = / { print "Name = " n; next } { print }' \
            "$btcfg" > "${btcfg}.tmp" 2>/dev/null
        if [ -s "${btcfg}.tmp" ] && mv "${btcfg}.tmp" "$btcfg" 2>/dev/null; then
            [ -n "$owner" ] && chown "$owner" "$btcfg" 2>/dev/null
            [ -n "$mode" ]  && chmod "$mode"  "$btcfg" 2>/dev/null
            log_ok "Rewrote Name in $btcfg"
            updated=1
        fi
        rm -f "${btcfg}.tmp" 2>/dev/null
    done
    [ "$updated" -eq 0 ] && log_warn "No writable bt_config.conf for Name"
    se_restore
    backup_rotate "bt_config_name." 10

    force_stop com.android.bluetooth
    pkill -f 'com\.(android|google\.android)\.bluetooth' 2>/dev/null
    sleep 1
    log_ok "Name: $NEW_NAME"
    return 0
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
}

cmd="${1:-all}"
shift 2>/dev/null || true
MODVER=$(awk -F= '$1=="version"{print $2}' "$MODDIR/module.prop" 2>/dev/null)
log_step "rotate_ids.sh cmd=$cmd (module $MODVER)"

case "$cmd" in
    all)
        wipe_ssaid           || FAILURES=$((FAILURES + 1))
        set_gaid_value "$@"  || FAILURES=$((FAILURES + 1))
        randomize_wlan_mac   || :
        rotate_bluetooth_mac || FAILURES=$((FAILURES + 1))
        sync_device_name "$@" || FAILURES=$((FAILURES + 1))
        sync_boot_count      || :
        ;;
    safe)
        set_gaid_value "$@"    || FAILURES=$((FAILURES + 1))
        rotate_bluetooth_mac   || FAILURES=$((FAILURES + 1))
        sync_device_name "$@"  || :
        sync_boot_count        || :
        ;;
    ssaid)                wipe_ssaid              || FAILURES=$((FAILURES + 1)) ;;
    gaid)                 set_gaid_value "$@"     || FAILURES=$((FAILURES + 1)) ;;
    wlan-mac|mac)         randomize_wlan_mac "$@" || FAILURES=$((FAILURES + 1)) ;;
    bt-mac|bluetooth-mac) rotate_bluetooth_mac "$@" || FAILURES=$((FAILURES + 1)) ;;
    device-name|name)     sync_device_name "$@"   || FAILURES=$((FAILURES + 1)) ;;
    boot-count|bootcount) sync_boot_count         || FAILURES=$((FAILURES + 1)) ;;
    status)               cmd_status ;;
    -h|--help|help)
        cat <<USAGE
Usage: rotate_ids.sh <cmd> [args]
  all                        - SSAID + GAID + wlan-MAC + BT-MAC + device-name + boot-count (default)
  safe                       - GAID + BT-MAC + device-name + boot-count (no reboot, no wifi reset)
  ssaid                      - wipe settings_ssaid.xml (needs reboot)
  gaid [uuid]                - set Google Advertising ID
  wlan-mac [xx:xx:..]        - set wlan0 MAC
  bt-mac [xx:xx:..]          - set Bluetooth adapter MAC
  device-name [name]         - sync device_name/BT to persona (identity.prop MODEL)
  boot-count                 - write Settings.Global.boot_count from identity.prop BOOT_COUNT
  status                     - show current values (read-only)
USAGE
        exit 0 ;;
    *) log_err "Unknown cmd: $cmd (try: rotate_ids.sh help)"; exit 2 ;;
esac

[ "$REBOOT_NEEDED" = "1" ] && log_warn "REBOOT REQUIRED for SSAID regeneration."
if [ "$FAILURES" -gt 0 ]; then
    log_warn "rotate_ids.sh: $FAILURES step(s) reported failure"
    exit 1
fi
log_ok "rotate_ids.sh $cmd completed cleanly"
exit 0
