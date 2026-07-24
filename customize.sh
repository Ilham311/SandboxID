#!/system/bin/sh
SKIPUNZIP=0

ui_print "- Ternak TT v1.0"
ui_print "- TikTok-focused Zygisk fresh persona"
ui_print ""

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

ui_print ""
ui_print "- Install complete. Reboot then tap Action to freshen."
