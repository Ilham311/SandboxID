#!/system/bin/sh
SKIPUNZIP=0

ui_print "- Ternak TT v1.1.7"
ui_print "- + FIX v1.1.7: swapped Dobby -> ShadowHook (bytedance/android-inline-hook)"
ui_print "-   Dobby master broke on NDK r26d (ADRP relocation, load_address"
ui_print "-   rename, missing Cpu.h). ShadowHook is actively maintained by"
ui_print "-   ByteDance, has the same call surface, and integrates cleanly."
ui_print "-   liblsplant.so + libternak_shadowhook.so shipped via system/lib{,64}."
ui_print "- + FIX v1.1.7: javac -bootclasspath android.jar (v1.1.6 helper dex was"
ui_print "-   empty because android.content.ContentResolver could not resolve)."
ui_print "- + FIX v1.1.6: AAR-based lsplant (Maven Central) + system/lib{,64}"
ui_print "-   overlay ship — Path B .so now resolvable inside app processes"
ui_print "-   (DT_NEEDED works via magic mount)."
ui_print "- + FIX v1.1.3: early bail-out for root/system/shell apps"
ui_print "- + FIX v1.1.4: build fix — <sys/socket.h> + forward decl (CI green)"
ui_print "-   (skips companion IPC for KSU, Magisk, Shizuku, Termux, ...)"
ui_print "-   fixes Android 15 Instrumentation-null NPE race on BOOT_COMPLETED"
ui_print "- + 500ms socket timeout on companion IPC (belt-and-suspenders)"
ui_print "- TikTok + Grab Zygisk fresh persona"
ui_print "- + Path B FULL: lsplant Java method hooks (5 hooks live)"
ui_print "-   * Settings.Secure.getString  -> android_id spoof"
ui_print "-   * Settings.Global.getString  -> dev_settings/adb spoof"
ui_print "-   * Settings.Global.getInt     -> boot_count/dev_mode spoof"
ui_print "-   * SystemClock.uptimeMillis   -> +offset"
ui_print "-   * SystemClock.elapsedRealtime -> +offset"
ui_print "- + L8: TimeZone.setDefault() + Locale.setDefault() JNI spoof"
ui_print "- + kernel identity bind (proc_uptime + kernel_boot_id)"
ui_print "- + rich BIND-FAIL diagnostic (src/dst stat + errno + strerror)"
ui_print "- + runtime target.txt (edit whitelist, no rebuild)"
ui_print "- + companion hot-reloads target.txt on mtime change"
ui_print "- + `ternak-tt targets` CLI to view whitelist"
ui_print "- + mount overlay (build.prop x5 + settings_secure.xml + proc_uptime + kernel_boot_id)"
ui_print "- + crash watchdog + auto-summarize on Action tap"
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
[ -f $MODPATH/post-fs-data.sh ] && set_perm $MODPATH/post-fs-data.sh 0 0 0755
[ -f $MODPATH/summarize.sh ] && set_perm $MODPATH/summarize.sh 0 0 0755
[ -f $MODPATH/target.txt ] && set_perm $MODPATH/target.txt 0 0 0644

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

# v1.1.7 Path B: pick correct per-ABI liblsplant.so + libternak_shadowhook.so,
# rename to .so, delete the other ABI variants. If path_b libs were not
# shipped (Path B disabled build), this block is a no-op.
if [ -f "$MODPATH/.path_b_stamp" ]; then
    ui_print "- Path B: installing lsplant + shadowhook for $ABI"
    case "$ABI" in
        arm64-v8a|x86_64) LIBDIR="$MODPATH/system/lib64" ;;
        armeabi-v7a|x86) LIBDIR="$MODPATH/system/lib"   ;;
        *)               LIBDIR="" ;;
    esac
    if [ -n "$LIBDIR" ] && [ -d "$LIBDIR" ]; then
        for LIB in liblsplant libternak_shadowhook; do
            SRC="$LIBDIR/$LIB.so.$ABI"
            if [ -f "$SRC" ]; then
                mv -f "$SRC" "$LIBDIR/$LIB.so"
                ui_print "-   $LIB.so ($(du -h "$LIBDIR/$LIB.so" | cut -f1))"
            else
                ui_print "! Path B: $LIB.so.$ABI not found — Java hooks will fail"
            fi
        done
        # Delete all other ABI variants and stray system/lib{,64} dirs
        for D in "$MODPATH/system/lib64" "$MODPATH/system/lib"; do
            [ "$D" = "$LIBDIR" ] && continue
            rm -rf "$D"
        done
        find "$LIBDIR" -name '*.so.*' -type f -delete
        set_perm_recursive "$MODPATH/system" 0 0 0755 0644
    else
        ui_print "! Path B: no lib dir for ABI $ABI — removing system/ overlay"
        rm -rf "$MODPATH/system"
    fi
    rm -f "$MODPATH/.path_b_stamp"
fi

echo "fresh" > $MODPATH/identity.mode
set_perm $MODPATH/identity.mode 0 0 0644

mkdir -p $MODPATH/mount/system
mkdir -p $MODPATH/mount/vendor
mkdir -p $MODPATH/mount/odm
mkdir -p $MODPATH/mount/product
mkdir -p $MODPATH/mount/system_ext
set_perm_recursive $MODPATH/mount 0 0 0755 0644

ui_print ""
ui_print "- Install complete. Reboot then tap Action to freshen."
