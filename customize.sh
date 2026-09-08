#!/system/bin/sh
# shellcheck disable=SC2034
SKIPUNZIP=0

SBX_VER=$(grep '^version=' "$MODPATH/module.prop" 2>/dev/null | cut -d= -f2)
ui_print "- SandboxID ${SBX_VER:-(versi ?)}"
ui_print "- Membuat aplikasi melihat perangkat ini sebagai perangkat lain:"
ui_print "-   model, brand, pabrikan, fingerprint, serial"
ui_print "-   plus entropy persona module-local (bukan SSAID sistem)"
ui_print "- Identitas perangkat diacak dari banyak brand:"
ui_print "-   Pixel, Samsung, Xiaomi, POCO, OPPO, vivo, Redmi, Infinix"
ui_print "-   peluang tiap brand sama rata."
ui_print "- Spoof berjalan saat proses aplikasi target dimulai."
ui_print "- Action hanya mengganti persona module-local secara atomik."
ui_print "-   Action tidak menghapus data/izin aplikasi, mereset SSAID,"
ui_print "-   menjalankan rotasi agregat, atau menerbitkan properti global."
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
    : > "$MODPATH/.operational-flags"
    for KEY in SBX_NATIVE_READ SBX_HIDE SBX_CPU_REVISION SBX_PROC_VERSION SBX_MEMINFO SBX_SYSFS_MAC; do
        VALUE=$(awk -F= -v k="$KEY" '$1==k && ($2=="0" || $2=="1") {v=$2} END {if (v!="") print v}' "$LIVE_IDENTITY" 2>/dev/null)
        [ -n "$VALUE" ] && printf '%s=%s\n' "$KEY" "$VALUE" >> "$MODPATH/.operational-flags"
    done
    if [ -s "$MODPATH/.operational-flags" ]; then
        ui_print "- Flag operasional dari instalasi sebelumnya akan dipulihkan saat boot"
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
case "$ABI" in
    arm64-v8a|armeabi-v7a|x86_64|x86) RP_ASSET="resetprop-$ABI" ;;
    *) abort "! ABI tidak didukung resetprop-rs v0.6.0: $ABI" ;;
esac

[ -f "$MODPATH/bin/$RP_ASSET" ] || abort "! Binary resetprop-rs untuk $ABI tidak ada"
[ -f "$MODPATH/bin/resetprop-rs.sha256" ] || abort "! Manifest checksum resetprop-rs tidak ada"
command -v sha256sum >/dev/null 2>&1 || abort "! sha256sum diperlukan untuk verifikasi resetprop-rs"
EXPECTED=$(grep "  $RP_ASSET\$" "$MODPATH/bin/resetprop-rs.sha256" | cut -d' ' -f1)
[ -n "$EXPECTED" ] || abort "! Checksum $RP_ASSET tidak terdaftar"
ACTUAL=$(sha256sum "$MODPATH/bin/$RP_ASSET" | cut -d' ' -f1)
[ "$ACTUAL" = "$EXPECTED" ] || abort "! Checksum resetprop-rs $ABI tidak cocok"
mv -f "$MODPATH/bin/$RP_ASSET" "$MODPATH/bin/resetprop-rs"
rm -f "$MODPATH/bin/resetprop-arm64-v8a" "$MODPATH/bin/resetprop-armeabi-v7a" \
      "$MODPATH/bin/resetprop-x86_64" "$MODPATH/bin/resetprop-x86"
set_perm "$MODPATH/bin/resetprop-rs" 0 0 0755
ui_print "- resetprop-rs v0.6.0 ($ABI) terverifikasi"

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
ui_print "- Selesai dipasang. Reboot, lalu tekan Action untuk mengacak perangkat baru."
