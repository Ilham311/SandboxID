#!/system/bin/sh
# shellcheck disable=SC2034
SKIPUNZIP=0

SBX_VER=$(grep '^version=' "$MODPATH/module.prop" 2>/dev/null | cut -d= -f2)
ui_print "- SandboxID ${SBX_VER:-(versi ?)}"
ui_print "- Persona disimpan lokal di modul dan disajikan hanya ke proses target:"
ui_print "-   Build.*, properti terpilih, dan view build.prop per-target"
ui_print "- Catatan: SandboxID tidak menghapus atau menulis penyimpanan SSAID."
ui_print "- Identitas perangkat diacak dari banyak brand:"
ui_print "-   Pixel, Samsung, Xiaomi, POCO, OPPO, vivo, Redmi, Infinix"
ui_print "-   peluang tiap brand sama rata."
ui_print "- Lifecycle normal tidak memublikasikan properti global atau menulis Settings."
ui_print "- Kemampuan runtime dan attestation hardware tetap asli."
ui_print "- /proc version, meminfo, sysfs MAC, agregat CPU, dan hide properti luas"
ui_print "-   bersifat opt-in, parsial, dan default-nonaktif."
ui_print "- Tombol Action hanya mengganti persona lokal; data aplikasi tidak dihapus."
ui_print "- Operasi shared-device di WebUI/rotate_ids.sh selalu individual dan manual."
ui_print "- Aplikasi target diatur sendiri di target.txt (kosong = modul nonaktif)."
ui_print "- WebUI: buka modul ini di manajer KernelSU/APatch."
ui_print ""

LIVE_TARGET="/data/adb/modules/sandboxid/target.txt"
if [ -s "$LIVE_TARGET" ]; then
    ui_print "- target.txt dari instalasi sebelumnya dipertahankan"
    cp -f "$LIVE_TARGET" "$MODPATH/target.txt"
else
    ui_print "- Menyiapkan target.txt (kosong dulu; isi nama paket aplikasi untuk mengaktifkan)"
fi

LIVE_CARRIER="/data/adb/modules/sandboxid/carrier.conf"
if [ -s "$LIVE_CARRIER" ]; then
    ui_print "- carrier.conf (pilihan operator) dari instalasi sebelumnya dipertahankan"
    cp -f "$LIVE_CARRIER" "$MODPATH/carrier.conf"
fi

LIVE_IDENTITY="/data/adb/modules/sandboxid/identity.prop"
if [ -r "$LIVE_IDENTITY" ]; then
    cp -f "$LIVE_IDENTITY" "$MODPATH/identity.prop"
    ui_print "- identity.prop dari instalasi sebelumnya dipertahankan"
    : > "$MODPATH/.operational-flags"
    for key in SBX_NATIVE_READ SBX_HIDE SBX_CPU_REVISION \
               SBX_PROC_VERSION SBX_MEMINFO SBX_SYSFS_MAC; do
        value=$(awk -F= -v k="$key" '$1==k { sub(/^[^=]*=/, ""); print; exit }' "$LIVE_IDENTITY" 2>/dev/null)
        case "$value" in
            0|1) printf '%s=%s\n' "$key" "$value" >> "$MODPATH/.operational-flags" ;;
        esac
    done
    if [ -s "$MODPATH/.operational-flags" ]; then
        ui_print "- Pengaturan native-read eksperimental dari instalasi sebelumnya dipertahankan"
    else
        rm -f "$MODPATH/.operational-flags"
    fi
fi

if [ -f "$MODPATH/debug_variant" ]; then
    ui_print "- ! Varian DEBUG terdeteksi"
    ui_print "-   Auto-log aktif pada boot berikutnya."
    ui_print "-   Lokasi: /data/adb/modules/sandboxid/debug/"
    ui_print "-   Pola nama file: session-YYYYMMDD-HHMMSS.log"
    ui_print "-   Tekan Action untuk menyimpan ringkasan log terbaru"
    ui_print "-   ke $MODPATH/debug/report/ (khusus root)"
    ui_print ""
fi

ABI=$(getprop ro.product.cpu.abi)
ui_print "- ABI perangkat: $ABI"

[ -d /data/adb/modules ] || abort "! root tidak terdeteksi"

ZOK=0
[ -d /data/adb/modules/zygisksu ] && ZOK=1
[ -d /data/adb/modules/ReZygisk ] && ZOK=1
[ "${MAGISK_VER_CODE:-0}" -ge 26100 ] && ZOK=1
[ "$ZOK" = "0" ] && ui_print "! PERHATIAN: Zygisk tidak terdeteksi — pasang ZygiskNext / ReZygisk dulu"

set_perm_recursive $MODPATH 0 0 0755 0644
set_perm $MODPATH/action.sh                 0 0 0755
set_perm $MODPATH/service.sh                0 0 0755
[ -f $MODPATH/post-fs-data.sh ] && set_perm $MODPATH/post-fs-data.sh 0 0 0755
[ -f $MODPATH/summarize.sh ] && set_perm $MODPATH/summarize.sh 0 0 0755
[ -f $MODPATH/helpers.sh ] && set_perm $MODPATH/helpers.sh 0 0 0644
[ -f $MODPATH/rotate_ids.sh ] && set_perm $MODPATH/rotate_ids.sh 0 0 0755
[ -f $MODPATH/selftest.sh ] && set_perm $MODPATH/selftest.sh 0 0 0755
[ -f $MODPATH/autopif.sh ] && set_perm $MODPATH/autopif.sh 0 0 0755
[ -f $MODPATH/personas.tsv ] && set_perm $MODPATH/personas.tsv 0 0 0644
[ -f $MODPATH/devices.tsv ] && set_perm $MODPATH/devices.tsv 0 0 0644
[ -f $MODPATH/carriers.tsv ] && set_perm $MODPATH/carriers.tsv 0 0 0644
[ -f $MODPATH/carrier.conf ] && set_perm $MODPATH/carrier.conf 0 0 0644
[ -f $MODPATH/target.txt ] && set_perm $MODPATH/target.txt 0 0 0644

mkdir -p "$MODPATH/backups"
set_perm $MODPATH/backups 0 0 0700

if [ -f "$MODPATH/debug_variant" ]; then
    mkdir -p "$MODPATH/debug"
    set_perm_recursive $MODPATH/debug 0 0 0755 0644
    set_perm $MODPATH/debug_variant 0 0 0644
fi
[ -d $MODPATH/webroot ] && set_perm_recursive $MODPATH/webroot 0 0 0755 0644
set_perm $MODPATH/bin/sandboxid-arm64       0 0 0755
set_perm $MODPATH/bin/sandboxid-arm         0 0 0755
set_perm $MODPATH/bin/sandboxid-x86_64      0 0 0755
set_perm $MODPATH/bin/sandboxid-x86         0 0 0755
if [ -f $MODPATH/bin/resetprop-rs ]; then
    set_perm $MODPATH/bin/resetprop-rs 0 0 0755

    if [ -f "$MODPATH/bin/resetprop-rs.sha256" ] && command -v sha256sum >/dev/null 2>&1; then
        if ( cd "$MODPATH/bin" && sha256sum -c resetprop-rs.sha256 >/dev/null 2>&1 ); then
            ui_print "- Checksum resetprop-rs OK"
        else
            ui_print "! Checksum resetprop-rs tidak cocok — binary bawaan dihapus"
            rm -f "$MODPATH/bin/resetprop-rs"
        fi
    fi

    if [ -f "$MODPATH/bin/resetprop-rs" ] && [ "$ABI" != "arm64-v8a" ]; then
        rm -f "$MODPATH/bin/resetprop-rs" "$MODPATH/bin/resetprop-rs.sha256"
        ui_print "- Catatan: resetprop-rs hanya untuk arm64 — dihapus di $ABI (pakai resetprop Magisk)."
    fi
fi

case "$ABI" in
    arm64-v8a)   ln -sf sandboxid-arm64  $MODPATH/bin/sandboxid ;;
    armeabi-v7a) ln -sf sandboxid-arm    $MODPATH/bin/sandboxid ;;
    x86_64)      ln -sf sandboxid-x86_64 $MODPATH/bin/sandboxid ;;
    x86)         ln -sf sandboxid-x86    $MODPATH/bin/sandboxid ;;
    *)           ui_print "! ABI tidak dikenal: $ABI" ;;
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
ui_print "- Selesai dipasang. Reboot sekali untuk memulihkan state properti ROM asli,"
ui_print "- lalu tekan Action dan buka ulang aplikasi target secara manual."
