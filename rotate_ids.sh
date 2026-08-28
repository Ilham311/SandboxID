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

# Regenerate ByteDance AppLog SDK identifier caches (did / iid / ssid /
# openudid / clientudid / cdid) for one or all packages in target.txt. Full
# cycle: wipe old cache → generate plausible new values (Snowflake did/iid/
# ssid + UUID cdid/clientudid + 16-hex openudid) → seed them into applog.xml,
# snssdk_openudid.xml, bd_device_info.xml, files/bd_setting/*, files/.cdid
# with correct ownership + SELinux context so the app reads them on next
# cold start as if they were its own persistent state.
#
# Heavy lifting is in helpers.sh::applog_regen; this wrapper handles arg
# dispatch and the "no target.txt / no arg" edge case (which is a user
# workflow issue, not a failure — exit clean with a hint).
#
# Historical alias 'wipe_applog' → still works via case dispatch below;
# it always meant "regenerate", never "just wipe" (wipe alone would leave
# the app in a zero-value gap until it re-registered with the server).
regen_applog() {
    _arg="${1:-}"
    log_step "Regenerate ByteDance AppLog IDs${_arg:+ ($_arg)}"

    if [ -n "$_arg" ]; then
        applog_regen "$_arg"
        return $?
    fi

    # No package given → walk target.txt. applog_regen handles the read itself
    # but returns 1 if the file is empty; surface that as a friendly hint.
    if ! grep -qE '^[[:space:]]*[^[:space:]#]' "$MODDIR/target.txt" 2>/dev/null; then
        log_info "target.txt kosong — tidak ada aplikasi ByteDance yang diregenerasi"
        log_info "Hint: 'rotate_ids.sh applog <package>' atau isi target.txt dulu"
        return 0
    fi
    applog_regen
}

# Escape hatch: wipe-only, no seed. Used by `rotate_ids.sh applog-wipe` for
# forensic scenarios (you want to see how the app re-registers from scratch
# with the server) or for apps that break when they find our seeded values.
# Regular users should stick with `regen_applog` above.
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

# Pilih operator/kartu SIM. Menulis carrier.conf (bertahan lintas rotasi persona,
# di-merge ulang oleh `sandboxid freshen`) DAN langsung menuliskan key GSM_* ke
# identity.prop supava efektif pada spawn app berikutnya tanpa freshen penuh.
#   set_carrier "MCC|MNC|NAMA|ISO|PHANTOM|CARRIER_ID"  -> pilih operator (ISO/PHANTOM/CARRIER_ID opsional)
#   set_carrier off                          -> hapus pilihan (kembali ke bawaan)
#   set_carrier status                       -> tampilkan pilihan sekarang
# PHANTOM=1 memaksa gsm.sim.state=LOADED: slot kosong ikut melaporkan SIM ada.
# CARRIER_ID = id carrier Android (getSimCarrierId). Bila kosong pada spec, diambil
# dari carriers.tsv sesuai MCC+MNC; tetap kosong bila operator tak ada di daftar.
_carrier_clear_keys() {
    identity_del GSM_OPERATOR_NUMERIC
    identity_del GSM_OPERATOR_ALPHA
    identity_del GSM_OPERATOR_ISO
    identity_del GSM_CARRIER_ID
    identity_del GSM_SIM_STATE
}

# Cari carrier_id (kolom ke-5 carriers.tsv) untuk baris MCC+MNC yang cocok.
# Kosong bila tak ada di daftar — di perangkat nyata getSimCarrierId() = -1 (UNKNOWN).
_carrier_lookup_id() {
    _lk_mcc="$1"; _lk_mnc="$2"
    [ -r "$CARRIERS_FILE" ] || return 0
    awk -F'\t' -v mcc="$_lk_mcc" -v mnc="$_lk_mnc" '
        /^[[:space:]]*#/ || NF < 4 { next }
        $2 == mcc && $3 == mnc { gsub(/[[:space:]]/, "", $5); print $5; exit }
    ' "$CARRIERS_FILE" 2>/dev/null
}

set_carrier() {
    spec="${1:-status}"
    case "$spec" in
        status|'')
            log_step "Status operator / kartu SIM"
            if [ -f "$CARRIER_CONF" ]; then
                cn="$(awk -F= '$1=="NAME"{sub(/^[^=]*=/,"");print;exit}' "$CARRIER_CONF" 2>/dev/null)"
                cm="$(awk -F= '$1=="MCC"{print $2;exit}'  "$CARRIER_CONF" 2>/dev/null)"
                cc="$(awk -F= '$1=="MNC"{print $2;exit}'  "$CARRIER_CONF" 2>/dev/null)"
                ci="$(awk -F= '$1=="ISO"{print $2;exit}'  "$CARRIER_CONF" 2>/dev/null)"
                cid="$(awk -F= '$1=="CARRIER_ID"{print $2;exit}' "$CARRIER_CONF" 2>/dev/null)"
                cp="$(awk -F= '$1=="PHANTOM"{print $2;exit}' "$CARRIER_CONF" 2>/dev/null)"
                log_info "Operator : ${cn:-(kosong)}"
                log_info "Kode     : ${cm}${cc}"
                log_info "Negara   : ${ci:-(kosong)}"
                log_info "Carrier ID: ${cid:-(kosong, -1/UNKNOWN)}"
                [ "$cp" = "1" ] && log_info "Phantom  : aktif (slot kosong lapor SIM ada)"
            else
                log_info "Belum ada operator dipilih — pakai bawaan (Telkomsel 51010)"
            fi
            log_info "identity GSM_OPERATOR_NUMERIC = $(identity_get GSM_OPERATOR_NUMERIC 2>/dev/null || true)"
            log_info "identity GSM_OPERATOR_ALPHA   = $(identity_get GSM_OPERATOR_ALPHA 2>/dev/null || true)"
            return 0 ;;
        off|none|clear|default)
            log_step "Nonaktifkan operator kustom"
            rm -f "$CARRIER_CONF" 2>/dev/null
            _carrier_clear_keys
            log_ok "Operator kustom dihapus — kembali ke bawaan"
            return 0 ;;
    esac

    mcc="$(printf '%s' "$spec" | awk -F'|' '{print $1}' | tr -d ' \t\r')"
    mnc="$(printf '%s' "$spec" | awk -F'|' '{print $2}' | tr -d ' \t\r')"
    name="$(printf '%s' "$spec" | awk -F'|' '{print $3}' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
    iso="$(printf '%s'  "$spec" | awk -F'|' '{print $4}' | tr -d ' \t\r' | tr '[:upper:]' '[:lower:]')"
    phantom="$(printf '%s' "$spec" | awk -F'|' '{print $5}' | tr -d ' \t\r')"
    [ "$phantom" = "1" ] || phantom=0
    cid="$(printf '%s' "$spec" | awk -F'|' '{print $6}' | tr -d ' \t\r')"

    case "$mcc" in ''|*[!0-9]*) log_err "carrier: MCC tidak valid ('$mcc') — harus 3 digit angka"; return 2 ;; esac
    case "$mnc" in ''|*[!0-9]*) log_err "carrier: MNC tidak valid ('$mnc') — harus 2-3 digit angka"; return 2 ;; esac
    if [ "${#mcc}" -ne 3 ]; then log_err "carrier: MCC harus 3 digit (dapat '$mcc')"; return 2; fi
    if [ "${#mnc}" -lt 2 ] || [ "${#mnc}" -gt 3 ]; then log_err "carrier: MNC harus 2-3 digit (dapat '$mnc')"; return 2; fi
    if [ -z "$name" ]; then log_err "carrier: nama operator kosong"; return 2; fi

    # carrier_id: pakai yang di spec; bila kosong, coba ambil dari carriers.tsv.
    [ -z "$cid" ] && cid="$(_carrier_lookup_id "$mcc" "$mnc" | tr -d ' \t\r')"
    # Validasi: kosong ATAU integer (boleh -1 UNKNOWN). Nilai aneh diabaikan (jangan tebak).
    case "$cid" in
        ''|-1) : ;;
        -[0-9]*|[0-9]*) case "${cid#-}" in *[!0-9]*) log_warn "carrier: CARRIER_ID '$cid' bukan angka — diabaikan"; cid="" ;; esac ;;
        *) log_warn "carrier: CARRIER_ID '$cid' bukan angka — diabaikan"; cid="" ;;
    esac

    _ph_note=""
    [ "$phantom" = "1" ] && _ph_note=", phantom"
    log_step "Set operator: $name ($mcc$mnc${iso:+, $iso}$_ph_note)"

    umask 077
    tmpc="${CARRIER_CONF}.tmp.$$"
    {
        printf 'NAME=%s\n' "$name"
        printf 'MCC=%s\n'  "$mcc"
        printf 'MNC=%s\n'  "$mnc"
        printf 'ISO=%s\n'  "$iso"
        printf 'PHANTOM=%s\n' "$phantom"
        printf 'CARRIER_ID=%s\n' "$cid"
    } > "$tmpc" 2>/dev/null || { log_err "carrier: gagal tulis carrier.conf"; rm -f "$tmpc"; umask 022; return 1; }
    mv "$tmpc" "$CARRIER_CONF" 2>/dev/null || { log_err "carrier: gagal simpan carrier.conf"; rm -f "$tmpc"; umask 022; return 1; }
    chmod 0644 "$CARRIER_CONF" 2>/dev/null
    umask 022

    # Terapkan langsung ke identity.prop (efektif pada spawn app berikutnya).
    identity_persist GSM_OPERATOR_NUMERIC "$mcc$mnc" || log_warn "carrier: gagal tulis GSM_OPERATOR_NUMERIC"
    identity_persist GSM_OPERATOR_ALPHA   "$name"    || log_warn "carrier: gagal tulis GSM_OPERATOR_ALPHA"
    if [ -n "$iso" ]; then identity_persist GSM_OPERATOR_ISO "$iso"; else identity_del GSM_OPERATOR_ISO; fi
    if [ -n "$cid" ]; then identity_persist GSM_CARRIER_ID "$cid"; else identity_del GSM_CARRIER_ID; fi
    if [ "$phantom" = "1" ]; then identity_persist GSM_SIM_STATE "LOADED"; else identity_del GSM_SIM_STATE; fi

    log_ok "Operator aktif: $name ($mcc$mnc${cid:+, id $cid}) — berlaku saat aplikasi target dibuka lagi"
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

    # AppLog snapshot — for each active target, show whether the SDK cache
    # files exist and the state (fresh/seeded/active/absent). We deliberately
    # DON'T dump the contents: did/iid/ssid are user-linkable identifiers and
    # this log ends up in /cache/*.log. See helpers.sh::applog_probe.
    if grep -qE '^[[:space:]]*[^[:space:]#]' "$MODDIR/target.txt" 2>/dev/null; then
        log_info "AppLog cache (per target):"
        while IFS= read -r _t || [ -n "$_t" ]; do
            _t=${_t%%#*}
            _t=$(printf '%s' "$_t" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
            [ -n "$_t" ] || continue
            _probe=$(applog_probe "$_t" 2>/dev/null)
            if [ -n "$_probe" ]; then
                # probe format: "<pkg> <count> <state>"
                _n=$(printf '%s' "$_probe" | awk '{print $2}')
                _st=$(printf '%s' "$_probe" | awk '{print $3}')
                log_info "  $_t = $_n file(s), state=$_st"
            else
                log_info "  $_t = (probe unavailable)"
            fi
        done < "$MODDIR/target.txt"
    fi
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
        # AppLog SDK regen runs LAST on purpose: force_stop inside applog_regen
        # kicks the target app so it re-reads the seeded did/iid/ssid on next
        # open, and by this point every hardware-layer identifier the SDK
        # would sample (Build.*, MAC, ANDROID_ID, GAID) has already been
        # rotated. If regen ran first, the app could be relaunched by the
        # user before the other rotations landed and re-register with the
        # stale hardware fingerprint on top of our fresh AppLog IDs.
        regen_applog         || :
        ;;
    safe)
        set_gaid_value "$@"    || FAILURES=$((FAILURES + 1))
        rotate_bluetooth_mac   || FAILURES=$((FAILURES + 1))
        sync_device_name "$@"  || :
        sync_boot_count        || :
        regen_applog           || :
        ;;
    ssaid)                wipe_ssaid              || FAILURES=$((FAILURES + 1)) ;;
    gaid)                 set_gaid_value "$@"     || FAILURES=$((FAILURES + 1)) ;;
    wlan-mac|mac)         randomize_wlan_mac "$@" || FAILURES=$((FAILURES + 1)) ;;
    bt-mac|bluetooth-mac) rotate_bluetooth_mac "$@" || FAILURES=$((FAILURES + 1)) ;;
    device-name|name)     sync_device_name "$@"   || FAILURES=$((FAILURES + 1)) ;;
    boot-count|bootcount) sync_boot_count         || FAILURES=$((FAILURES + 1)) ;;
    carrier|sim)          set_carrier "$@"        || FAILURES=$((FAILURES + 1)) ;;
    # 'applog' = full regen (wipe+generate+seed). 'applog-wipe' preserved as
    # an escape hatch: wipe-only for forensic-scenarios or unusual apps where
    # you want the SDK to re-register from scratch against the server rather
    # than reading our seeded values.
    applog|bytedance|regen-applog) regen_applog "$@"       || FAILURES=$((FAILURES + 1)) ;;
    applog-wipe|wipe-applog)       wipe_applog_only "$@"   || FAILURES=$((FAILURES + 1)) ;;
    status)               cmd_status ;;
    -h|--help|help)
        cat <<USAGE
Usage: rotate_ids.sh <cmd> [args]
  all                        - SSAID + GAID + wlan-MAC + BT-MAC + device-name + boot-count + applog (default)
  safe                       - GAID + BT-MAC + device-name + boot-count + applog (no reboot, no wifi reset)
  ssaid                      - wipe settings_ssaid.xml (needs reboot)
  gaid [uuid]                - set Google Advertising ID
  wlan-mac [xx:xx:..]        - set wlan0 MAC
  bt-mac [xx:xx:..]          - set Bluetooth adapter MAC
  device-name [name]         - sync device_name/BT to persona (identity.prop MODEL)
  boot-count                 - write Settings.Global.boot_count from identity.prop BOOT_COUNT
  carrier <spec>|off|status  - pick SIM/operator; spec = "MCC|MNC|NAME|ISO|PHANTOM|CARRIER_ID"
  applog [pkg]               - regenerate ByteDance AppLog cache (wipe old + generate + seed new
                               did/iid/ssid/openudid/clientudid/cdid) for one package, or every
                               package in target.txt
  applog-wipe [pkg]          - wipe only (no seed) — forces SDK to re-register from server
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
