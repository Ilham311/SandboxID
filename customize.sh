#!/system/bin/sh
SKIPUNZIP=0

ui_print "- Ternak TT v1.0.15"
ui_print "- TikTok + Grab Zygisk fresh persona"
ui_print "- + runtime target.txt (edit whitelist, no rebuild)"
ui_print "- + companion hot-reloads target.txt on mtime change"
ui_print "- + `ternak-tt targets` CLI to view whitelist"
ui_print "- + L7 SUPPRESS label for log.looper.*.slow (log noise)"
ui_print "- + summarize.sh: SPOOF broken out by L1/L2/L7-SPB/SPI/SPL"
ui_print "- + mount overlay (build.prop x5 + settings_secure.xml)"
ui_print "- + crash watchdog + auto-summarize on Action tap"
ui_print ""

# v1.0.15: preserve user's custom target.txt across reinstalls.
# KernelSU / Magisk stage the new module under MODPATH and swap it in
# on reboot. If the live install already has target.txt, copy it into
# MODPATH BEFORE ui_print reports so the user's edits survive upgrade.
LIVE_TARGET="/data/adb/modules/ternak_tt/target.txt"
if [ -s "$LIVE_TARGET" ]; then
    ui_print "- Preserving existing target.txt from previous install"
    cp -f "$LIVE_TARGET" "$MODPATH/target.txt"
else
    ui_print "- Installing default target.txt (4 packages)"
fi

# Detect debug variant marker (dropped by build.sh)
if [ -f "$MODPATH/debug_variant" ]; then
    ui_print "- ! DEBUG variant detected"
    ui_print "-   Auto-log will start on next boot."
    ui_print "-   Location: /data/adb/modules/ternak_tt/debug/"
    ui_print "-   File name: session-<timestamp>.log"
    ui_print "-   Tap Action button to snapshot latest log"
    ui_print "-   to /sdcard/Download/ternak-tt-logs/"
    ui_print ""
fi

ABI=$(getprop ro.product.cpu.abi)
ui_print "- Device ABI: $ABI"

[ -d /data/adb/modules ] || abort "! root not detected"

ZOK=0
[ -d /data/adb/modules/zygisksu ] && ZOK=1
[ -d /data/adb/modules/ReZygisk ] && ZOK=1
[ "${MAGISK_VER_CODE:-0}" -ge 26100 ] && ZOK=1
[ "$ZOK" = "0" ] && ui_print "! WARNING: Zygisk not detected — install ZygiskNext / ReZygisk first"

set_perm_recursive $MODPATH 0 0 0755 0644
set_perm $MODPATH/action.sh                 0 0 0755
set_perm $MODPATH/service.sh                0 0 0755
# v1.0.14: post-fs-data.sh runs early to seed mount overlay before Zygisk.
[ -f $MODPATH/post-fs-data.sh ] && set_perm $MODPATH/post-fs-data.sh 0 0 0755
[ -f $MODPATH/summarize.sh ] && set_perm $MODPATH/summarize.sh 0 0 0755
# v1.0.15: target.txt must be world-readable (companion runs as root so
# 0644 is fine; user can `su -c 'nano /data/adb/modules/ternak_tt/target.txt'`).
[ -f $MODPATH/target.txt ] && set_perm $MODPATH/target.txt 0 0 0644

# v1.0.10: prepare debug/ dir so background logcat can write on first boot
if [ -f "$MODPATH/debug_variant" ]; then
    mkdir -p "$MODPATH/debug"
    set_perm_recursive $MODPATH/debug 0 0 0755 0644
    set_perm $MODPATH/debug_variant 0 0 0644
fi
set_perm $MODPATH/bin/ternak-tt-arm64       0 0 0755
set_perm $MODPATH/bin/ternak-tt-arm         0 0 0755
set_perm $MODPATH/bin/ternak-tt-x86_64      0 0 0755
set_perm $MODPATH/bin/ternak-tt-x86         0 0 0755
[ -f $MODPATH/bin/resetprop-rs ] && set_perm $MODPATH/bin/resetprop-rs 0 0 0755

case "$ABI" in
    arm64-v8a)   ln -sf ternak-tt-arm64  $MODPATH/bin/ternak-tt ;;
    armeabi-v7a) ln -sf ternak-tt-arm    $MODPATH/bin/ternak-tt ;;
    x86_64)      ln -sf ternak-tt-x86_64 $MODPATH/bin/ternak-tt ;;
    x86)         ln -sf ternak-tt-x86    $MODPATH/bin/ternak-tt ;;
    *)           ui_print "! Unknown ABI: $ABI" ;;
esac

echo "fresh" > $MODPATH/identity.mode
set_perm $MODPATH/identity.mode 0 0 0644

# v1.0.3: pre-create mount overlay tree so bind-mount sources exist
# (real content generated on first `ternak-tt freshen`)
mkdir -p $MODPATH/mount/system
mkdir -p $MODPATH/mount/vendor
mkdir -p $MODPATH/mount/odm
mkdir -p $MODPATH/mount/product
mkdir -p $MODPATH/mount/system_ext
set_perm_recursive $MODPATH/mount 0 0 0755 0644

ui_print ""
ui_print "- Install complete. Reboot then tap Action to freshen."
