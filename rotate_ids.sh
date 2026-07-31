#!/system/bin/sh
# rotate_ids.sh - Ternak TT shell-layer identity rotation.
#
# v1.1.0 refactor:
#   - set_gaid_value       -> write_adid_settings_xml + apply_gaid_props + restart_gms_optional
#   - rotate_bluetooth_mac -> bt_config_rewrite_field("Address",...) helper + apply_bt_mac_props + bt_stack_kick
#   - sync_device_name     -> bt_config_rewrite_field("Name",...)    helper + apply_device_name_props + bt_stack_kick
# The two bt_config helpers moved to helpers.sh (bt_config_rewrite_field / bt_stack_kick).

MODDIR="${MODDIR:-/data/adb/modules/ternak_tt}"
LOGFILE="${LOGFILE:-/cache/ternak-tt-boot.log}"

. "$MODDIR/helpers.sh" 2>/dev/null || { echo "[rotate_ids] helpers.sh missing"; exit 1; }

# ---- SSAID wipe ------------------------------------------------------------
rotate_ssaid() {
    log_step "SSAID wipe"
    se_permissive
    for u in $(get_users); do
        db="/data/system/users/$u/settings_ssaid.xml"
        if [ -f "$db" ]; then
            cp -f "$db" "$BACKUP_DIR_ROOT/settings_ssaid.$u.$(date +%s).xml" 2>/dev/null
            echo '<?xml version="1.0" encoding="utf-8" standalone="yes" ?><settings version="1"></settings>' > "$db" 2>/dev/null && \
                log_ok "cleared $db"
        fi
    done
    backup_rotate "settings_ssaid." 10
    se_restore
}

# ---- GAID ------------------------------------------------------------------
write_adid_settings_xml() {
    new_gaid="$1"
    se_permissive
    for u in $(get_users); do
        gms="/data/user/$u/com.google.android.gms/shared_prefs/adid_settings.xml"
        [ -d "$(dirname "$gms")" ] || continue
        cat > "$gms" <<XML 2>/dev/null
<?xml version='1.0' encoding='utf-8' standalone='yes' ?>
<map>
    <string name="adid_key">$new_gaid</string>
    <boolean name="limit_ad_tracking" value="false" />
</map>
XML
        chown 10001:10001 "$gms" 2>/dev/null
        chmod 0660 "$gms" 2>/dev/null
        log_ok "wrote $gms"
    done
    se_restore
}

apply_gaid_props() {
    val="$1"
    rp_set "persist.ad.id" "$val"
    rp_set "persist.gaid"  "$val"
    identity_persist "GAID" "$val"
}

restart_gms_optional() {
    force_stop com.google.android.gms 2>/dev/null || true
    force_stop com.google.android.gsf 2>/dev/null || true
}

set_gaid_value() {
    NEW_GAID="${1:-$(generate_uuid)}"
    log_step "GAID rotation -> $NEW_GAID"
    write_adid_settings_xml "$NEW_GAID"
    apply_gaid_props        "$NEW_GAID"
    restart_gms_optional
}

rotate_gaid() { set_gaid_value "$(generate_uuid)"; }

# ---- WLAN MAC --------------------------------------------------------------
rotate_wlan_mac() {
    MAC="$(generate_mac)"
    log_step "WLAN MAC rotation -> $MAC"
    se_permissive
    for f in /data/misc/wifi/WifiConfigStore.xml \
             /data/misc/apexdata/com.android.wifi/WifiConfigStore.xml; do
        [ -f "$f" ] || continue
        cp -f "$f" "$BACKUP_DIR_ROOT/wifi_config.$(date +%s).xml" 2>/dev/null
        sed -i.bak -E 's/(MacAddress|WifiConfigStoreMacAddress).*/\1 name="WifiConfigStoreMacAddress" value="'"$MAC"'" \//g' "$f" 2>/dev/null
        rm -f "$f.bak" 2>/dev/null
        log_ok "rewrote $f"
    done
    backup_rotate "wifi_config." 10
    rp_set "persist.wifi.factory.mac" "$MAC"
    identity_persist "WIFI_MAC" "$MAC"
    se_restore
}

# ---- Bluetooth MAC ---------------------------------------------------------
apply_bt_mac_props() {
    mac="$1"
    rp_set "persist.bluetooth.address" "$mac"
    rp_set "ro.boot.btmacaddr"         "$mac"
    identity_persist "BT_MAC" "$mac"
}

rotate_bluetooth_mac() {
    MAC="$(generate_mac)"
    log_step "Bluetooth MAC rotation -> $MAC"
    se_permissive
    bt_config_rewrite_field "Address" "$MAC" "bt_config_addr." 1 || log_warn "no bt_config.conf found"
    apply_bt_mac_props "$MAC"
    bt_stack_kick
    se_restore
}

# ---- Device / BT name ------------------------------------------------------
apply_device_name_props() {
    name="$1"
    rp_set "net.hostname"           "$name"
    rp_set "persist.sys.device_name" "$name"
    settings_put global   device_name        "$name"
    settings_put secure   bluetooth_name     "$name"
    settings_put system   device_name        "$name"
    identity_persist "DEVICE_NAME" "$name"
}

sync_device_name() {
    NAME="${1:-}"
    if [ -z "$NAME" ]; then
        NAME="$(identity_get MODEL 2>/dev/null)"
    fi
    [ -z "$NAME" ] && { log_warn "no MODEL in identity - skipping device name sync"; return 0; }
    log_step "device/BT name sync -> $NAME"
    se_permissive
    bt_config_rewrite_field "Name" "$NAME" "bt_config_name." 0 || log_warn "no bt_config.conf carrying Name= (skipped)"
    apply_device_name_props "$NAME"
    bt_stack_kick
    se_restore
}

# ---- dispatch --------------------------------------------------------------
case "${1:-}" in
    ssaid) rotate_ssaid ;;
    gaid)  rotate_gaid ;;
    wlan)  rotate_wlan_mac ;;
    bt)    rotate_bluetooth_mac ;;
    name)  sync_device_name "$2" ;;
    all|"")
        rotate_ssaid
        rotate_gaid
        rotate_wlan_mac
        rotate_bluetooth_mac
        sync_device_name
        ;;
    *) echo "usage: $0 {all|ssaid|gaid|wlan|bt|name [NAME]}"; exit 2 ;;
esac
