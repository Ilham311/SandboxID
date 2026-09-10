#!/system/bin/sh
# shellcheck disable=SC2034
SKIPUNZIP=0

SBX_VER=$(grep '^version=' "$MODPATH/module.prop" 2>/dev/null | cut -d= -f2)
ui_print "- SandboxID ${SBX_VER:-(versi ?)}"
ui_print "- Satu Action menyiapkan persona exact-SDK, menerapkan presentasi,"
ui_print "-   mereset data target terpasang, merotasi penyimpanan ID lokal,"
ui_print "-   lalu memverifikasi hasil yang dapat dilanjutkan atau dipulihkan."
ui_print "- Katalog offline tervalidasi tersedia di binary; personas.tsv hanya ekstensi."
ui_print "- Setiap Action mencoba refresh Pixel OTA exact-SDK yang dibatasi; cache lama"
ui_print "-   dan katalog offline tetap dipakai bila jaringan atau validasi gagal."
ui_print "- Action dapat menghapus data aplikasi target dan mungkin perlu reboot."
ui_print "- Kegagalan pasca-commit dilaporkan parsial; bukan disamarkan sebagai sukses."
ui_print "- Properti presentasi diterapkan pre-Zygote; hook aktif saat target dibuat."
ui_print "- Kemampuan runtime, API layanan, dan attestation perangkat tetap asli."
ui_print "- Opsi /proc, meminfo, dan CPU eksperimental bersifat parsial."
ui_print "- Aplikasi target diatur di target.txt (file kosong = modul nonaktif)."
ui_print "- WebUI: buka modul ini di manajer KernelSU/APatch."
ui_print ""

LIVE_MODULE_DIR="${SBX_LIVE_MODULE_DIR:-/data/adb/modules/sandboxid}"
MODULES_ROOT="${SBX_MODULES_ROOT:-/data/adb/modules}"

regular_source() {
    [ -f "$1" ] && [ ! -L "$1" ] && [ -r "$1" ]
}

file_sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        _sha_line="$(sha256sum "$1" 2>/dev/null)" || return 1
    elif command -v toybox >/dev/null 2>&1; then
        _sha_line="$(toybox sha256sum "$1" 2>/dev/null)" || return 1
    else
        return 1
    fi
    printf '%s\n' "$_sha_line" | awk '{print $1}'
}

pair_hash_matches() {
    _pair_data="$1"
    _pair_meta="$2"
    _pair_key="$3"
    _pair_expected="$(awk -F= -v key="$_pair_key" '
        $1 == key { value=substr($0, index($0, "=") + 1); count++ }
        END { if (count != 1) exit 1; print value }
    ' "$_pair_meta" 2>/dev/null)" || return 1
    [ "${#_pair_expected}" -eq 64 ] || return 1
    case "$_pair_expected" in *[!0-9a-f]*) return 1 ;; esac
    _pair_actual="$(file_sha256 "$_pair_data")" || return 1
    [ "$_pair_actual" = "$_pair_expected" ]
}

preserve_file() {
    _label="$1"
    _source="$2"
    _destination="$3"
    regular_source "$_source" || return 1
    if ! cp -f "$_source" "$_destination"; then
        abort "! Gagal mempertahankan $_label; instalasi dibatalkan"
    fi
    ui_print "- $_label dari instalasi sebelumnya dipertahankan"
    return 0
}

LIVE_TARGET="$LIVE_MODULE_DIR/target.txt"
if ! preserve_file "target.txt" "$LIVE_TARGET" "$MODPATH/target.txt"; then
    ui_print "- Menyiapkan target.txt (kosong dulu; isi proses target untuk mengaktifkan)"
fi

LIVE_MODE="$LIVE_MODULE_DIR/identity.mode"
if ! preserve_file "identity.mode" "$LIVE_MODE" "$MODPATH/identity.mode"; then
    if ! printf '%s\n' fresh > "$MODPATH/identity.mode"; then
        abort "! Gagal membuat identity.mode"
    fi
    ui_print "- identity.mode baru disiapkan dalam mode fresh"
fi

preserve_validated_pair() {
    _pv_label="$1"
    _pv_data_name="$2"
    _pv_meta_name="$3"
    _pv_hash_key="$4"
    _pv_data="$LIVE_MODULE_DIR/$_pv_data_name"
    _pv_meta="$LIVE_MODULE_DIR/$_pv_meta_name"
    _pv_data_present=0
    _pv_meta_present=0
    if [ -e "$_pv_data" ] || [ -L "$_pv_data" ]; then
        _pv_data_present=1
    fi
    if [ -e "$_pv_meta" ] || [ -L "$_pv_meta" ]; then
        _pv_meta_present=1
    fi
    if [ "$_pv_data_present" -eq 0 ] && [ "$_pv_meta_present" -eq 0 ]; then
        return 1
    fi
    if [ "$_pv_data_present" -ne "$_pv_meta_present" ]; then
        return 2
    fi
    if ! regular_source "$_pv_data" || ! regular_source "$_pv_meta" ||
       ! pair_hash_matches "$_pv_data" "$_pv_meta" "$_pv_hash_key"; then
        return 3
    fi
    cp -f "$_pv_data" "$MODPATH/$_pv_data_name" ||
        abort "! Gagal mempertahankan data $_pv_label"
    if ! cp -f "$_pv_meta" "$MODPATH/$_pv_meta_name"; then
        rm -f "$MODPATH/$_pv_data_name"
        abort "! Gagal mempertahankan metadata $_pv_label"
    fi
    ui_print "- Pasangan $_pv_label tervalidasi dari instalasi sebelumnya dipertahankan"
    return 0
}

LIVE_IDENTITY="$LIVE_MODULE_DIR/identity.prop"
IDENTITY_PRESERVED=0
preserve_validated_pair "identity canonical" identity.prop identity.meta identity_sha256
_identity_pair_rc=$?
case "$_identity_pair_rc" in
    0) IDENTITY_PRESERVED=1 ;;
    1) : ;;
    2)
        if regular_source "$LIVE_IDENTITY" && [ ! -e "$LIVE_MODULE_DIR/identity.meta" ] &&
           [ ! -L "$LIVE_MODULE_DIR/identity.meta" ]; then
            preserve_file "identity.prop legacy (metadata akan dimigrasikan native)" \
                "$LIVE_IDENTITY" "$MODPATH/identity.prop" ||
                abort "! Gagal mempertahankan identity.prop legacy"
            IDENTITY_PRESERVED=1
        else
            abort "! Pasangan identity canonical instalasi lama tidak lengkap"
        fi ;;
    *) abort "! Pasangan identity canonical instalasi lama tidak valid" ;;
esac

preserve_optional_pair() {
    _po_label="$1"
    shift
    preserve_validated_pair "$_po_label" "$@"
    _po_rc=$?
    case "$_po_rc" in
        0|1) return 0 ;;
        2) abort "! Pasangan $_po_label instalasi lama tidak lengkap" ;;
        *) abort "! Pasangan $_po_label instalasi lama tidak valid" ;;
    esac
}

preserve_optional_pair "identity backup" identity.prop.bak identity.meta.bak identity_sha256
preserve_optional_pair "cache persona" persona.cache persona.cache.meta candidate_sha256

if [ "$IDENTITY_PRESERVED" -eq 1 ]; then
    if ! : > "$MODPATH/.operational-flags"; then
        abort "! Gagal menyiapkan pemulihan pengaturan operasional"
    fi
    for key in SBX_NATIVE_READ SBX_HIDE SBX_CPU_REVISION \
               SBX_PROC_VERSION SBX_MEMINFO; do
        value=$(awk -F= -v k="$key" '$1==k { sub(/^[^=]*=/, ""); print; exit }' "$LIVE_IDENTITY" 2>/dev/null)
        case "$value" in
            0|1) printf '%s=%s\n' "$key" "$value" >> "$MODPATH/.operational-flags" ||
                abort "! Gagal menyimpan pengaturan operasional $key" ;;
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

[ -d "$MODULES_ROOT" ] || abort "! root tidak terdeteksi"

ZOK=0
[ -d /data/adb/modules/zygisksu ] && ZOK=1
[ -d /data/adb/modules/ReZygisk ] && ZOK=1
[ "${MAGISK_VER_CODE:-0}" -ge 26100 ] && ZOK=1
[ "$ZOK" = "0" ] && ui_print "! PERHATIAN: Zygisk tidak terdeteksi — pasang ZygiskNext / ReZygisk dulu"

for _required in action.sh service.sh helpers.sh rotate_ids.sh autopif.sh target.txt \
                 bin/sandboxid-arm64 bin/sandboxid-arm bin/sandboxid-x86_64 \
                 bin/sandboxid-x86; do
    [ -e "$MODPATH/$_required" ] || abort "! File runtime wajib hilang: $_required"
done

set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/action.sh"                 0 0 0755
set_perm "$MODPATH/service.sh"                0 0 0755
[ -f "$MODPATH/post-fs-data.sh" ] && set_perm "$MODPATH/post-fs-data.sh" 0 0 0755
[ -f "$MODPATH/summarize.sh" ] && set_perm "$MODPATH/summarize.sh" 0 0 0755
set_perm "$MODPATH/helpers.sh"                0 0 0644
set_perm "$MODPATH/rotate_ids.sh"             0 0 0755
[ -f "$MODPATH/selftest.sh" ] && set_perm "$MODPATH/selftest.sh" 0 0 0755
[ -f "$MODPATH/autopif.sh" ] && set_perm "$MODPATH/autopif.sh" 0 0 0755
[ -f "$MODPATH/personas.tsv" ] && set_perm "$MODPATH/personas.tsv" 0 0 0644
[ -f "$MODPATH/target.txt" ] && set_perm "$MODPATH/target.txt" 0 0 0644
for _private_state in identity.prop identity.meta identity.prop.bak identity.meta.bak \
                      persona.cache persona.cache.meta; do
    [ -f "$MODPATH/$_private_state" ] && set_perm "$MODPATH/$_private_state" 0 0 0600
done

mkdir -p "$MODPATH/backups"
set_perm $MODPATH/backups 0 0 0700

if [ -f "$MODPATH/debug_variant" ]; then
    mkdir -p "$MODPATH/debug"
    set_perm_recursive $MODPATH/debug 0 0 0755 0644
    set_perm $MODPATH/debug_variant 0 0 0644
fi
[ -d "$MODPATH/webroot" ] && set_perm_recursive "$MODPATH/webroot" 0 0 0755 0644
set_perm "$MODPATH/bin/sandboxid-arm64"       0 0 0755
set_perm "$MODPATH/bin/sandboxid-arm"         0 0 0755
set_perm "$MODPATH/bin/sandboxid-x86_64"      0 0 0755
set_perm "$MODPATH/bin/sandboxid-x86"         0 0 0755
if [ -e "$MODPATH/bin/resetprop-rs" ] || [ -L "$MODPATH/bin/resetprop-rs" ] ||
   [ -e "$MODPATH/bin/resetprop-rs.sha256" ] || [ -L "$MODPATH/bin/resetprop-rs.sha256" ] ||
   [ -e "$MODPATH/bin/resetprop-rs.LICENSE" ] || [ -L "$MODPATH/bin/resetprop-rs.LICENSE" ]; then
    _resetprop_ok=1
    if [ ! -f "$MODPATH/bin/resetprop-rs" ] || [ -L "$MODPATH/bin/resetprop-rs" ] ||
       [ ! -f "$MODPATH/bin/resetprop-rs.sha256" ] || [ -L "$MODPATH/bin/resetprop-rs.sha256" ] ||
       [ ! -f "$MODPATH/bin/resetprop-rs.LICENSE" ] || [ -L "$MODPATH/bin/resetprop-rs.LICENSE" ]; then
        _resetprop_ok=0
        ui_print "! Set binary/checksum/lisensi resetprop-rs tidak lengkap atau bukan file biasa."
    elif [ "$ABI" != "arm64-v8a" ]; then
        _resetprop_ok=0
        ui_print "- resetprop-rs arm64 tidak digunakan pada $ABI; backend PATH diperlukan."
    elif [ ! -f "$MODPATH/bin/resetprop-rs.sha256" ]; then
        _resetprop_ok=0
        ui_print "! Checksum resetprop-rs tidak tersedia; binary bawaan ditolak."
    elif ! command -v sha256sum >/dev/null 2>&1; then
        _resetprop_ok=0
        ui_print "! sha256sum tidak tersedia; resetprop-rs tidak dapat diverifikasi dan ditolak."
    elif ! ( cd "$MODPATH/bin" && sha256sum -c resetprop-rs.sha256 >/dev/null 2>&1 ); then
        _resetprop_ok=0
        ui_print "! Checksum resetprop-rs tidak cocok; binary bawaan ditolak."
    fi
    if [ "$_resetprop_ok" = 1 ]; then
        set_perm "$MODPATH/bin/resetprop-rs" 0 0 0755
        ui_print "- Checksum resetprop-rs OK"
    else
        rm -f "$MODPATH/bin/resetprop-rs" "$MODPATH/bin/resetprop-rs.sha256" \
            "$MODPATH/bin/resetprop-rs.LICENSE" ||
            abort "! Gagal menyingkirkan resetprop-rs yang tidak terverifikasi"
        ui_print "- Property apply akan memakai resetprop/resetprop-rs dari PATH bila tersedia."
    fi
fi

case "$ABI" in
    arm64-v8a)   _sandboxid_binary=sandboxid-arm64 ;;
    armeabi-v7a) _sandboxid_binary=sandboxid-arm ;;
    x86_64)      _sandboxid_binary=sandboxid-x86_64 ;;
    x86)         _sandboxid_binary=sandboxid-x86 ;;
    *)           abort "! ABI tidak didukung: $ABI" ;;
esac
[ -f "$MODPATH/bin/$_sandboxid_binary" ] ||
    abort "! Binary CLI untuk ABI $ABI tidak tersedia"
set_perm "$MODPATH/bin/$_sandboxid_binary" 0 0 0755
ln -sf "$_sandboxid_binary" "$MODPATH/bin/sandboxid" ||
    abort "! Gagal memilih binary CLI untuk ABI $ABI"
[ -x "$MODPATH/bin/sandboxid" ] ||
    abort "! Binary CLI terpilih tidak executable untuk ABI $ABI"

set_perm "$MODPATH/identity.mode" 0 0 0644

mkdir -p $MODPATH/mount/system
mkdir -p $MODPATH/mount/vendor
mkdir -p $MODPATH/mount/odm
mkdir -p $MODPATH/mount/product
mkdir -p $MODPATH/mount/system_ext
set_perm_recursive $MODPATH/mount 0 0 0755 0644

ui_print ""
ui_print "- Selesai dipasang. Reboot, lalu tekan Action untuk mengacak perangkat baru."
