#!/system/bin/sh
SKIPUNZIP=0

TT_VER=$(grep '^version=' "$MODPATH/module.prop" 2>/dev/null | cut -d= -f2)
ui_print "- Ternak TT ${TT_VER:-(version unknown)}"
ui_print "- Spoofs device identity apps see:"
ui_print "-   model, brand, manufacturer, fingerprint, serial"
ui_print "-   per-app Android ID / SSAID"
ui_print "- Property spoof runs pre-zygote (before apps launch)"
ui_print "- Only safe identity strings - no risky HW/framework changes"
ui_print "- 1-tap Action: freshen -> rotate_ids all"
ui_print "-   SSAID wipe, GAID, wlan MAC, BT MAC, device/BT name"
ui_print "- Targets: TikTok + Grab Passenger (target.txt editable)"
ui_print "- WebUI: open module in KernelSU/APatch manager"
ui_print ""

LIVE_TARGET="/data/adb/modules/ternak_tt/target.txt"
if [ -s "$LIVE_TARGET" ]; then
    ui_print "- Preserving existing target.txt from previous install"
    cp -f "$LIVE_TARGET" "$MODPATH/target.txt"
else
    ui_print "- Installing default target.txt (4 packages)"
fi

if [ -f "$MODPATH/debug_variant" ]; then
    ui_print "- ! DEBUG variant detected"
    ui_print "-   Auto-log will start on next boot."
    ui_print "-   Location: /data/adb/modules/ternak_tt/debug/"
    ui_print "-   File name pattern: session-YYYYMMDD-HHMMSS.log"
    ui_print "-   Tap Action button to snapshot latest log"
    ui_print "-   to $MODPATH/debug/report/ (root-only)"
    ui_print ""
fi

ABI=$(getprop ro.product.cpu.abi)
ui_print "- Device ABI: $ABI"

[ -d /data/adb/modules ] || abort "! root not detected"

ZOK=0
[ -d /data/adb/modules/zygisksu ] && ZOK=1
[ -d /data/adb/modules/ReZygisk ] && ZOK=1
[ "${MAGISK_VER_CODE:-0}" -ge 26100 ] && ZOK=1
[ "$ZOK" = "0" ] && ui_print "! WARNING: Zygisk not detected - install ZygiskNext / ReZygisk first"

set_perm_recursive $MODPATH 0 0 0755 0644
set_perm $MODPATH/action.sh                 0 0 0755
set_perm $MODPATH/service.sh                0 0 0755
[ -f $MODPATH/post-fs-data.sh ] && set_perm $MODPATH/post-fs-data.sh 0 0 0755
[ -f $MODPATH/summarize.sh ] && set_perm $MODPATH/summarize.sh 0 0 0755
[ -f $MODPATH/helpers.sh ] && set_perm $MODPATH/helpers.sh 0 0 0644
[ -f $MODPATH/rotate_ids.sh ] && set_perm $MODPATH/rotate_ids.sh 0 0 0755
[ -f $MODPATH/target.txt ] && set_perm $MODPATH/target.txt 0 0 0644

mkdir -p "$MODPATH/backups"
set_perm $MODPATH/backups 0 0 0700

if [ -f "$MODPATH/debug_variant" ]; then
    mkdir -p "$MODPATH/debug"
    set_perm_recursive $MODPATH/debug 0 0 0755 0644
    set_perm $MODPATH/debug_variant 0 0 0644
fi
[ -d $MODPATH/webroot ] && set_perm_recursive $MODPATH/webroot 0 0 0755 0644
set_perm $MODPATH/bin/ternak-tt-arm64       0 0 0755
set_perm $MODPATH/bin/ternak-tt-arm         0 0 0755
set_perm $MODPATH/bin/ternak-tt-x86_64      0 0 0755
set_perm $MODPATH/bin/ternak-tt-x86         0 0 0755
if [ -f $MODPATH/bin/resetprop-rs ]; then
    set_perm $MODPATH/bin/resetprop-rs 0 0 0755
    # C1: verifikasi binary vendored terhadap checksum yang ikut dikemas; buang bila diubah.
    if [ -f "$MODPATH/bin/resetprop-rs.sha256" ] && command -v sha256sum >/dev/null 2>&1; then
        if ( cd "$MODPATH/bin" && sha256sum -c resetprop-rs.sha256 >/dev/null 2>&1 ); then
            ui_print "- resetprop-rs checksum OK"
        else
            ui_print "! resetprop-rs checksum MISMATCH - removing bundled binary"
            rm -f "$MODPATH/bin/resetprop-rs"
        fi
    fi
    # M6: resetprop-rs prebuilt = arm64-only. Di ABI lain tak bisa dieksekusi -
    #     buang, andalkan Magisk 'resetprop' (helpers.sh rp_set auto-deteksi).
    if [ -f "$MODPATH/bin/resetprop-rs" ] && [ "$ABI" != "arm64-v8a" ]; then
        rm -f "$MODPATH/bin/resetprop-rs" "$MODPATH/bin/resetprop-rs.sha256"
        ui_print "- Note: resetprop-rs is arm64-only; removed on $ABI (Magisk resetprop used)."
    fi
fi

case "$ABI" in
    arm64-v8a)   ln -sf ternak-tt-arm64  $MODPATH/bin/ternak-tt ;;
    armeabi-v7a) ln -sf ternak-tt-arm    $MODPATH/bin/ternak-tt ;;
    x86_64)      ln -sf ternak-tt-x86_64 $MODPATH/bin/ternak-tt ;;
    x86)         ln -sf ternak-tt-x86    $MODPATH/bin/ternak-tt ;;
    *)           ui_print "! Unknown ABI: $ABI" ;;
esac

echo "fresh" > $MODPATH/identity.mode
set_perm $MODPATH/identity.mode 0 0 0644

mkdir -p $MODPATH/mount/system
mkdir -p $MODPATH/mount/vendor
mkdir -p $MODPATH/mount/odm
mkdir -p $MODPATH/mount/product
mkdir -p $MODPATH/mount/system_ext
set_perm_recursive $MODPATH/mount 0 0 0755 0644

ui_print ""
ui_print "- Install complete. Reboot then tap Action for 1-shot rotation."
