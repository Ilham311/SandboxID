#!/system/bin/sh
#
# autopif.sh — pembikin persona SandboxID.
#
# Dua subcommand:
#
#   device   (default)  OFFLINE. Ambil SATU device asli secara ACAK ADIL dari
#                        devices.tsv — brand dipilih dulu (rata di semua brand:
#                        Samsung/Xiaomi/Redmi/POCO/vivo/OPPO/Infinix/Google),
#                        baru model di dalam brand itu. Jadi tiap brand punya
#                        peluang SAMA, nggak peduli berapa banyak modelnya. Lalu
#                        diramu identity Android yang utuh + nyambung (fingerprint,
#                        build prop, serial/AndroidID/GAID acak) plus riwayat pakai
#                        yang masuk akal (jml boot, lama nyala, status "fresh"
#                        yang dicek biar cocok sama umur device). Hasilnya
#                        ditampilkan + ditulis ke device.identity siap-pakai.
#                        Ini yang dipakai tombol Action.
#                        Alias: gen, multibrand, profile.
#
#   fetch    (online)    Best-effort: tarik SATU persona Pixel canary acak dari
#                        data build resmi Google, tulis sebagai persona.override
#                        satu-kali yang dikonsumsi `sandboxid freshen`. NO-OP
#                        (exit 0) kalau device nggak punya curl/wget. Ini path
#                        LAMA yang khusus Pixel — dibiarkan buat yang mau, tapi
#                        tombol Action nggak lagi memakainya (biar adil multibrand).

MODDIR="${MODDIR:-/data/adb/modules/sandboxid}"
OVERRIDE_FILE="${PERSONA_OVERRIDE:-$MODDIR/persona.override}"

# knob fetch (Pixel canary)
CANARY_RELEASE="${CANARY_RELEASE:-16}"
CANARY_SDK="${CANARY_SDK:-36}"
MAX_TRY="${AUTOPIF_MAX_TRY:-4}"

# knob device (multi-brand)
DEVICES_FILE="${AUTOPIF_DEVICES:-$MODDIR/devices.tsv}"
IDENTITY_ARTIFACT="${AUTOPIF_ARTIFACT:-$MODDIR/device.identity}"
GEN_MAX_TRY="${AUTOPIF_GEN_MAX_TRY:-8}"

log() { echo "[autopif] $*"; }

# --------------------------------------------------------------------------
# helper bareng
# --------------------------------------------------------------------------

# integer uniform di [0, n)
rand_below() {
  _n="$1"
  [ "$_n" -gt 0 ] 2>/dev/null || { echo 0; return; }
  _h=$(od -An -N4 -tx1 /dev/urandom 2>/dev/null | tr -d ' \n')
  if [ -n "$_h" ]; then _r=$(( (0x$_h) & 0x7fffffff )); else _r=$(( ($$ * 2654435761) & 0x7fffffff )); fi
  echo $(( _r % _n ))
}

# integer uniform di [lo, hi] inklusif
rand_range() {
  _lo="$1"; _hi="$2"
  [ "$_hi" -le "$_lo" ] 2>/dev/null && { echo "$_lo"; return; }
  echo $(( _lo + $(rand_below $(( _hi - _lo + 1 )) ) ))
}

# hex huruf-kecil sepanjang 2*nbytes dari /dev/urandom (fallback: seed pid)
rand_hex() {
  _nb="$1"
  _x=$(od -An -N"$_nb" -tx1 /dev/urandom 2>/dev/null | tr -d ' \n')
  if [ -z "$_x" ]; then
    _x=""
    _seed=$$
    _i=0
    while [ "$_i" -lt "$_nb" ]; do
      _seed=$(( (_seed * 1103515245 + 12345) & 0x7fffffff ))
      _x="$_x$(printf '%02x' $(( _seed & 0xff )))"
      _i=$(( _i + 1 ))
    done
  fi
  printf '%s' "$_x"
}

# UUID v4 (huruf kecil) — bentuk GAID/AAID
rand_uuid() {
  _a=$(rand_hex 4); _b=$(rand_hex 2); _c=$(rand_hex 2); _d=$(rand_hex 2); _e=$(rand_hex 6)
  _c="4$(printf '%s' "$_c" | cut -c2-4)"                       # versi 4
  _v=$(printf '89ab' | cut -c$(( $(rand_below 4) + 1 )))       # varian 10xx
  _d="${_v}$(printf '%s' "$_d" | cut -c2-4)"
  printf '%s-%s-%s-%s-%s' "$_a" "$_b" "$_c" "$_d" "$_e"
}

# buang satu nol depan biar dash/mksh nggak baca "08"/"09" sebagai oktal salah
strip0() { case "$1" in 0?) echo "${1#0}" ;; *) echo "$1" ;; esac; }

# hari sejak 1970-01-01 utk Y M D (Gregorian proleptik, algoritma Howard Hinnant).
# Semua input di sini pasca-1970 jadi cabang negatif referensi mati — dihapus
# biar tetap POSIX.
days_from_civil() {
  _y="$1"; _m="$2"; _d="$3"
  [ "$_m" -le 2 ] && _y=$(( _y - 1 ))
  _era=$(( _y / 400 ))
  _yoe=$(( _y - _era * 400 ))
  if [ "$_m" -gt 2 ]; then _mp=$(( _m - 3 )); else _mp=$(( _m + 9 )); fi
  _doy=$(( (153 * _mp + 2) / 5 + _d - 1 ))
  _doe=$(( _yoe * 365 + _yoe / 4 - _yoe / 100 + _doy ))
  echo $(( _era * 146097 + _doe - 719468 ))
}

# epoch (UTC 00:00) utk YYYY-MM atau YYYY-MM-DD (tanpa tanggal => tgl 15)
ymd_to_epoch() {
  _y=$(echo "$1" | cut -d- -f1)
  _mo=$(echo "$1" | cut -d- -f2)
  _da=$(echo "$1" | cut -d- -f3)
  [ -z "$_da" ] && _da=15
  _mo=$(strip0 "$_mo"); _da=$(strip0 "$_da")
  echo $(( $(days_from_civil "$_y" "$_mo" "$_da") * 86400 ))
}

# epoch -> YYYY-MM-DD (UTC). z selalu > 0 utk tanggal kita -> tanpa cabang negatif.
epoch_to_ymd() {
  _z=$(( $1 / 86400 + 719468 ))
  _era=$(( _z / 146097 ))
  _doe=$(( _z - _era * 146097 ))
  _yoe=$(( (_doe - _doe / 1460 + _doe / 36524 - _doe / 146096) / 365 ))
  _y=$(( _yoe + _era * 400 ))
  _doy=$(( _doe - (365 * _yoe + _yoe / 4 - _yoe / 100) ))
  _mp=$(( (5 * _doy + 2) / 153 ))
  _d=$(( _doy - (153 * _mp + 2) / 5 + 1 ))
  if [ "$_mp" -lt 10 ]; then _mo=$(( _mp + 3 )); else _mo=$(( _mp - 9 )); fi
  [ "$_mo" -le 2 ] && _y=$(( _y + 1 ))
  printf '%04d-%02d-%02d' "$_y" "$_mo" "$_d"
}

# detik -> "Xd Yh Zm"
fmt_dur() {
  _s="$1"
  printf '%dd %dh %dm' $(( _s / 86400 )) $(( (_s % 86400) / 3600 )) $(( (_s % 3600) / 60 ))
}

# sekarang (epoch). AUTOPIF_NOW meng-override buat tes yang bisa diulang.
now_epoch() {
  if [ -n "$AUTOPIF_NOW" ]; then echo "$AUTOPIF_NOW"; return; fi
  _n=$(date +%s 2>/dev/null)
  case "$_n" in ''|*[!0-9]*) _n=$(ymd_to_epoch "2025-01-01") ;; esac
  echo "$_n"
}

download() {
  _url="$1"; _out="$2"; _ref="$3"
  if [ "$DL" = "curl" ]; then
    if [ -n "$_ref" ]; then
      curl -sL --connect-timeout 10 --max-time 40 -H "Referer: $_ref" "$_url" -o "$_out" 2>/dev/null
    else
      curl -sL --connect-timeout 10 --max-time 40 "$_url" -o "$_out" 2>/dev/null
    fi
  else
    if [ -n "$_ref" ]; then
      wget -T 10 -qO "$_out" --header "Referer: $_ref" "$_url" 2>/dev/null
    else
      wget -T 10 -qO "$_out" "$_url" 2>/dev/null
    fi
  fi
}

make_tmp() {
  TMP_DIR=$(mktemp -d 2>/dev/null)
  if [ -z "$TMP_DIR" ] || [ ! -d "$TMP_DIR" ]; then
    TMP_DIR="${TMPDIR:-/data/local/tmp}/autopif.$$"
    mkdir -p "$TMP_DIR" 2>/dev/null || return 1
  fi
  return 0
}

# --------------------------------------------------------------------------
# subcommand: device  (generator identity + riwayat multi-brand, offline)
# --------------------------------------------------------------------------

# release Android yang diharapkan utk sebuah SDK (penjaga konsistensi)
sdk_release() {
  case "$1" in
    30) echo "11" ;; 31) echo "12" ;; 32) echo "12" ;;
    33) echo "13" ;; 34) echo "14" ;; 35) echo "15" ;; 36) echo "16" ;;
    *)  echo "" ;;
  esac
}

# security-patch & tanggal rilis representatif utk sebuah SDK. Dipakai saat
# LOCK_SDK/LOCK_REL aktif (assemble_identity) biar SECPATCH/RELEASE_DATE ikut
# sinkron ke Android yang DIPAKSA, bukan ketinggalan di Android asli baris
# device yang dipilih.
sdk_secpatch() {
  case "$1" in
    30) echo "2021-08-05" ;; 31) echo "2022-08-05" ;; 32) echo "2022-08-05" ;;
    33) echo "2023-08-05" ;; 34) echo "2024-08-05" ;; 35) echo "2024-11-05" ;;
    36) echo "2025-08-05" ;;
    *)  echo "" ;;
  esac
}
sdk_release_date() {
  case "$1" in
    30) echo "2020-09" ;; 31) echo "2021-10" ;; 32) echo "2022-07" ;;
    33) echo "2022-08" ;; 34) echo "2023-10" ;; 35) echo "2024-10" ;;
    36) echo "2025-06" ;;
    *)  echo "" ;;
  esac
}

col() { printf '%s' "$1" | cut -f"$2"; }

# ramu jml-boot / lama-nyala / status-fresh yang nyambung dari tgl rilis model.
# Set global: AGE_DAYS OWNED_DAYS BOOT_COUNT UPTIME_S FIRST_BOOT LAST_BOOT_EP
#             FRESH PROFILE DPB10 RESET
gen_lifecycle() {
  _rel="$1"; _now="$2"
  _rel_ep=$(ymd_to_epoch "$_rel")
  _age_days=$(( (_now - _rel_ep) / 86400 ))
  [ "$_age_days" -lt 1 ] && _age_days=$(rand_range 1 30)     # rilis masa depan/baru => anggap baru
  [ "$_age_days" -gt 1825 ] && _age_days=1825                # batasi umur absurd (~5th)

  # ~18% baru factory-reset (jual-beli/servis/identitas baru — skenario modul ini
  # sendiri): waktu pakai menciut ke hitungan hari & BOOT_COUNT mulai rendah lagi.
  _reset=0
  if [ "$(rand_below 100)" -lt 18 ]; then
    _reset=1
    _owned_days=$(rand_range 1 25)
    [ "$_owned_days" -gt "$_age_days" ] && _owned_days=$_age_days
  elif [ "$_age_days" -le 45 ]; then
    _owned_days=$(rand_range 1 "$_age_days")
  else
    _pct=$(rand_range 50 100)                                 # dimiliki 50–100% umur model
    _owned_days=$(( _age_days * _pct / 100 ))
    [ "$_owned_days" -lt 1 ] && _owned_days=1
  fi

  _dpb10=$(rand_range 30 100)                                 # 3.0–10.0 hari/boot (x10)
  _setup_boots=$(rand_range 2 5)                              # boot setup awal + OTA awal
  _boot_count=$(( _owned_days * 10 / _dpb10 + _setup_boots ))
  [ "$_boot_count" -lt 1 ] && _boot_count=1
  [ "$_boot_count" -gt 1500 ] && _boot_count=1500

  _dpb=$(( _dpb10 / 10 )); [ "$_dpb" -lt 1 ] && _dpb=1
  _up_cap_days=$(( _dpb * 3 / 2 + 1 ))
  [ "$_up_cap_days" -gt "$_owned_days" ] && _up_cap_days=$_owned_days
  [ "$_up_cap_days" -gt 21 ] && _up_cap_days=21
  [ "$_up_cap_days" -lt 1 ] && _up_cap_days=1
  _up_max_s=$(( _up_cap_days * 86400 ))
  _a=$(rand_range 1200 "$_up_max_s"); _b=$(rand_range 1200 "$_up_max_s")
  if [ "$_a" -le "$_b" ]; then _uptime_s=$_a; else _uptime_s=$_b; fi   # condong pendek

  _first_boot_ep=$(( _now - _owned_days * 86400 ))
  _last_boot_ep=$(( _now - _uptime_s ))

  if [ "$_reset" -eq 1 ] || [ "$_owned_days" -lt 21 ] || [ "$_boot_count" -lt 8 ]; then
    _fresh="yes"; _profile="fresh"
  elif [ "$_owned_days" -gt 540 ] || [ "$_boot_count" -gt 160 ]; then
    _fresh="no"; _profile="seasoned"
  else
    _fresh="no"; _profile="active"
  fi

  AGE_DAYS=$_age_days; OWNED_DAYS=$_owned_days; BOOT_COUNT=$_boot_count
  UPTIME_S=$_uptime_s; FIRST_BOOT=$(epoch_to_ymd "$_first_boot_ep")
  LAST_BOOT_EP=$_last_boot_ep; FRESH=$_fresh; PROFILE=$_profile
  DPB10=$_dpb10; RESET=$_reset
}

# cek invarian riwayat; kalau gagal tambahkan alasan ke ERRMSG & return nonzero.
# Baca global dari gen_lifecycle. Panggil LANGSUNG (bukan di $(...)) biar lihat
# global shell induk.
validate_lifecycle() {
  _now="$1"; _f=0
  [ "$AGE_DAYS" -ge 1 ] || { ERRMSG="$ERRMSG bad-age($AGE_DAYS)"; _f=1; }
  { [ "$OWNED_DAYS" -ge 1 ] && [ "$OWNED_DAYS" -le "$AGE_DAYS" ]; } || { ERRMSG="$ERRMSG owned-not-in-[1,age]"; _f=1; }
  [ "$BOOT_COUNT" -ge 1 ] || { ERRMSG="$ERRMSG boot<1"; _f=1; }
  _os=$(( OWNED_DAYS * 86400 ))
  { [ "$UPTIME_S" -ge 1 ] && [ "$UPTIME_S" -le "$_os" ]; } || { ERRMSG="$ERRMSG uptime-not-in-[1,owned]"; _f=1; }
  [ "$UPTIME_S" -le 1814400 ] || { ERRMSG="$ERRMSG uptime>21d"; _f=1; }
  [ "$LAST_BOOT_EP" -ge "$(( _now - _os ))" ] || { ERRMSG="$ERRMSG last_boot<first_boot"; _f=1; }
  return $_f
}

# bangun KV identity lengkap (kosakata identity.prop native) ke IDENTITY_KV, dari
# satu baris devices.tsv. Kalau error tambahkan alasan ke ERRMSG & return nonzero.
# Set banyak global -> WAJIB dipanggil langsung, bukan di dalam $(...).
assemble_identity() {
  _row="$1"
  BRAND=$(col "$_row" 1);        MANUFACTURER=$(col "$_row" 2)
  MARKETNAME=$(col "$_row" 3);   MODEL=$(col "$_row" 4)
  DEVICE=$(col "$_row" 5);       PRODUCT=$(col "$_row" 6)
  BOARD=$(col "$_row" 7);        SOC_MANUF=$(col "$_row" 8)
  SOC_MODEL=$(col "$_row" 9);    SDK=$(col "$_row" 10)
  RELEASE=$(col "$_row" 11);     BUILD_ID=$(col "$_row" 12)
  INCREMENTAL=$(col "$_row" 13); SECPATCH=$(col "$_row" 14)
  RELEASE_DATE=$(col "$_row" 15)

  # Kunci versi (dari cmd_device): kalau di-set, paksa SDK/RELEASE ke Android asli
  # perangkat. FINGERPRINT di bawah dibangun ulang dari $RELEASE, jadi ikut sinkron.
  # SECPATCH/RELEASE_DATE baris asal masih nempel ke SDK/RELEASE ASLI baris itu —
  # kalau dibiarkan, metadata build (patch level & tanggal rilis) nggak nyambung
  # sama RELEASE yang dipaksa. Timpa juga keduanya ke nilai representatif SDK
  # yang dipaksa biar semua field build tetap konsisten satu sama lain.
  if [ -n "${LOCK_SDK:-}" ]; then
    SDK=$LOCK_SDK
    _lock_secpatch=$(sdk_secpatch "$LOCK_SDK"); [ -n "$_lock_secpatch" ] && SECPATCH="$_lock_secpatch"
    _lock_reldate=$(sdk_release_date "$LOCK_SDK"); [ -n "$_lock_reldate" ] && RELEASE_DATE="$_lock_reldate"
  fi
  [ -n "${LOCK_REL:-}" ] && RELEASE=$LOCK_REL

  for _v in "$BRAND" "$MANUFACTURER" "$MODEL" "$DEVICE" "$PRODUCT" "$BOARD" \
            "$SOC_MODEL" "$SDK" "$RELEASE" "$BUILD_ID" "$INCREMENTAL" \
            "$SECPATCH" "$RELEASE_DATE"; do
    [ -n "$_v" ] || { ERRMSG="$ERRMSG empty-required-field"; return 1; }
  done
  case "$SDK" in ''|*[!0-9]*) ERRMSG="$ERRMSG non-numeric-sdk"; return 1 ;; esac
  case "$SECPATCH" in
    [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]) : ;;
    *) ERRMSG="$ERRMSG secpatch-not-YYYY-MM-DD"; return 1 ;;
  esac
  _exp=$(sdk_release "$SDK")
  if [ -n "$_exp" ] && [ "$_exp" != "$RELEASE" ]; then
    ERRMSG="$ERRMSG sdk$SDK-expects-rel$_exp-got$RELEASE"; return 1
  fi

  FINGERPRINT="$BRAND/$PRODUCT/$DEVICE:$RELEASE/$BUILD_ID/$INCREMENTAL:user/release-keys"
  DESCRIPTION="$PRODUCT-user $RELEASE $BUILD_ID $INCREMENTAL release-keys"
  SERIAL=$(rand_hex 8)
  ANDROID_ID=$(rand_hex 8)
  GAID=$(rand_uuid)
  HOSTN="$(printf '%s' "$BRAND" | tr '[:upper:]' '[:lower:]')-build-$(rand_range 100 999)"

  # Kill switch rebuild-free: kalau ada file penanda $MODDIR/no_uptime, jangan kirim
  # UPTIME_SECONDS asli (kirim 0). Native install_uptime_hook nge-skip saat
  # UPTIME_SECONDS<=0, jadi apps balik ke uptime beneran TANPA rebuild/reflash.
  # (UPTIME_S internal tetap utuh biar validate_lifecycle lolos.)
  _uptime_emit=$UPTIME_S
  [ -f "$MODDIR/no_uptime" ] && _uptime_emit=0

  # native baca balik key ini apa adanya (load_identity) & menerapkannya; key
  # ekstra (BOOT_COUNT, UPTIME_*, ...) diabaikan apply_native, aman sbg metadata.
  IDENTITY_KV=$(cat <<EOF
BRAND=$BRAND
MANUFACTURER=$MANUFACTURER
MODEL=$MODEL
MARKETNAME=$MARKETNAME
DEVICE=$DEVICE
PRODUCT=$PRODUCT
BOARD=$BOARD
HARDWARE=$BOARD
BOARD_PLATFORM=$BOARD
SOC_MANUFACTURER=$SOC_MANUF
SOC_MODEL=$SOC_MODEL
FINGERPRINT=$FINGERPRINT
ID=$BUILD_ID
DISPLAY=$BUILD_ID
DESCRIPTION=$DESCRIPTION
BOOTLOADER=unknown
HOST=$HOSTN
USER=builder
TYPE=user
TAGS=release-keys
INCREMENTAL=$INCREMENTAL
RELEASE=$RELEASE
SDK_INT=$SDK
SECURITY_PATCH=$SECPATCH
SERIAL=$SERIAL
ANDROID_ID=$ANDROID_ID
GOOGLE_AID=$GAID
RELEASE_DATE=$RELEASE_DATE
AGE_DAYS=$AGE_DAYS
OWNED_DAYS=$OWNED_DAYS
BOOT_COUNT=$BOOT_COUNT
UPTIME_SECONDS=$_uptime_emit
UPTIME_HUMAN=$(fmt_dur "$UPTIME_S")
FIRST_BOOT=$FIRST_BOOT
LAST_BOOT=$(epoch_to_ymd "$LAST_BOOT_EP")
USAGE_PROFILE=$PROFILE
FRESH=$FRESH
EOF
)
  return 0
}

display_profile() {
  echo ""
  echo "  ┌─ Profil device SandboxID ──────────────────────────────"
  printf '  │ %-13s %s\n' "Brand"        "$BRAND"
  printf '  │ %-13s %s\n' "Pabrikan"     "$MANUFACTURER"
  printf '  │ %-13s %s (%s)\n' "Model"    "$MARKETNAME" "$MODEL"
  printf '  │ %-13s %s / %s\n' "Kode"     "$DEVICE" "$PRODUCT"
  printf '  │ %-13s %s %s (%s)\n' "SoC"    "$SOC_MANUF" "$SOC_MODEL" "$BOARD"
  printf '  │ %-13s Android %s (SDK %s), patch %s\n' "OS" "$RELEASE" "$SDK" "$SECPATCH"
  printf '  │ %-13s %s\n' "Fingerprint" "$FINGERPRINT"
  echo "  ├─ riwayat pakai (nyambung rilis $RELEASE_DATE) ──────────"
  printf '  │ %-13s %s  (~1 reboot / %s.%s hari)\n' "Jml boot"  "$BOOT_COUNT" "$(( DPB10 / 10 ))" "$(( DPB10 % 10 ))"
  printf '  │ %-13s %s  (%ss)\n' "Lama nyala"  "$(fmt_dur "$UPTIME_S")" "$UPTIME_S"
  printf '  │ %-13s %s\n' "Boot awal"  "$FIRST_BOOT"
  printf '  │ %-13s %s  (umur %sh, dipakai %sh)\n' "Pemakaian"  "$PROFILE" "$AGE_DAYS" "$OWNED_DAYS"
  printf '  │ %-13s %s%s\n' "Fresh"  "$FRESH" "$( [ "$RESET" -eq 1 ] && echo '  (baru factory-reset)' )"
  echo "  ├─ acak per-identitas ───────────────────────────────────"
  printf '  │ %-13s %s\n' "Serial"      "$SERIAL"
  printf '  │ %-13s %s\n' "Android ID"  "$ANDROID_ID"
  printf '  │ %-13s %s\n' "GAID"        "$GAID"
  echo "  └─────────────────────────────────────────────────────────"
  echo ""
}

cmd_device() {
  NOW=$(now_epoch)

  if [ ! -r "$DEVICES_FILE" ]; then
    log "database device nggak ketemu di $DEVICES_FILE — nggak ada yang bisa dibikin"
    return 0
  fi
  make_tmp || { log "nggak ada folder temp yang bisa ditulis — batal"; return 0; }
  cleanup_dev() { rm -rf "$TMP_DIR" 2>/dev/null; }
  trap cleanup_dev EXIT

  RAW_ALL="$TMP_DIR/devices.all"
  grep -v '^[[:space:]]*#' "$DEVICES_FILE" 2>/dev/null | grep -v '^[[:space:]]*$' > "$RAW_ALL"
  total_all=$(wc -l < "$RAW_ALL" 2>/dev/null | tr -d ' ')
  case "$total_all" in ''|*[!0-9]*) total_all=0 ;; esac
  if [ "$total_all" -eq 0 ]; then
    log "database device kosong setelah difilter — nggak ada yang bisa dibikin"
    return 0
  fi

  # ── KUNCI VERSI ANDROID ke perangkat ────────────────────────────────────────
  # Native memasang Build.VERSION.SDK_INT/RELEASE dari persona apa adanya
  # (main.cpp set_int/set_str + hook properti). Kalau SDK persona != Android asli
  # hp, app bisa error (lihat kebijakan native pick_persona() di sandboxid.cpp yg
  # sudah mencocokkan persona ke SDK perangkat). Jalur multibrand ini dulu belum.
  # Aturan: utamakan model yang MEMANG rilis di Android ini; kalau pool nggak
  # punya, tetap ambil model lain TAPI paksa SDK/RELEASE ke versi asli perangkat.
  RAW="$TMP_DIR/devices.raw"
  LOCK_SDK=""; LOCK_REL=""
  _dev_sdk="${SBX_REAL_SDK:-$(getprop ro.build.version.sdk 2>/dev/null || :)}"
  _dev_rel="${SBX_REAL_RELEASE:-$(getprop ro.build.version.release 2>/dev/null || :)}"
  case "$_dev_sdk" in ''|*[!0-9]*) _dev_sdk="" ;; esac
  if [ -n "$_dev_sdk" ]; then
    awk -F'\t' -v s="$_dev_sdk" '$10==s' "$RAW_ALL" > "$RAW"
    _nmatch=$(wc -l < "$RAW" 2>/dev/null | tr -d ' ')
    case "$_nmatch" in ''|*[!0-9]*) _nmatch=0 ;; esac
    if [ "$_nmatch" -ge 1 ]; then
      log "kunci versi: Android ${_dev_rel:-?} (SDK $_dev_sdk) — $_nmatch model bawaan versi ini dipakai"
    else
      cp -f "$RAW_ALL" "$RAW" 2>/dev/null
      LOCK_SDK="$_dev_sdk"
      LOCK_REL=$(sdk_release "$_dev_sdk"); [ -z "$LOCK_REL" ] && LOCK_REL="$_dev_rel"
      log "kunci versi: nggak ada model bawaan Android ${_dev_rel:-$_dev_sdk} di pool — model lain dipakai, SDK/RELEASE dipaksa ke SDK $_dev_sdk"
    fi
  else
    cp -f "$RAW_ALL" "$RAW" 2>/dev/null
    log "kunci versi dilewat: versi Android perangkat nggak kebaca (getprop)"
  fi

  total=$(wc -l < "$RAW" 2>/dev/null | tr -d ' ')
  case "$total" in ''|*[!0-9]*) total=0 ;; esac
  if [ "$total" -eq 0 ]; then
    log "pool device kosong setelah kunci versi — batal"
    return 0
  fi

  # ACAK ADIL 2 TAHAP. Tahap 1: kumpulkan brand unik (kolom 1) lalu pilih SATU
  # brand rata. Tahap 2: pilih satu model rata di dalam brand itu. Ini inti
  # permintaan "biar semua brand kebagian" — brand berbanyak model (Google=14)
  # nggak lebih sering muncul dari yang sedikit (POCO=2).
  BRANDS="$TMP_DIR/brands.lst"
  cut -f1 "$RAW" | awk 'NF && !seen[$0]++' > "$BRANDS"
  nbrands=$(wc -l < "$BRANDS" 2>/dev/null | tr -d ' ')
  case "$nbrands" in ''|*[!0-9]*) nbrands=0 ;; esac
  if [ "$nbrands" -lt 1 ]; then
    log "nggak nemu brand apa pun di database — batal"
    return 0
  fi

  try=0
  ok=0
  while [ "$try" -lt "$GEN_MAX_TRY" ]; do
    try=$(( try + 1 ))

    # tahap 1 — pilih brand rata
    bidx=$(( $(rand_below "$nbrands") + 1 ))
    brand=$(sed -n "${bidx}p" "$BRANDS")
    [ -z "$brand" ] && continue

    # tahap 2 — pilih 1 model di dalam brand itu, rata
    BROWS="$TMP_DIR/brand.rows"
    awk -F'\t' -v b="$brand" '$1==b' "$RAW" > "$BROWS"
    bcount=$(wc -l < "$BROWS" 2>/dev/null | tr -d ' ')
    case "$bcount" in ''|*[!0-9]*) bcount=0 ;; esac
    [ "$bcount" -lt 1 ] && continue
    ridx=$(( $(rand_below "$bcount") + 1 ))
    row=$(sed -n "${ridx}p" "$BROWS")
    [ -z "$row" ] && continue

    RELEASE_DATE_PRE=$(col "$row" 15)
    [ -z "$RELEASE_DATE_PRE" ] && continue
    gen_lifecycle "$RELEASE_DATE_PRE" "$NOW"
    # CATATAN: assemble_identity / validate_lifecycle men-set global, jadi WAJIB
    # jalan di shell INI. Kalau di $(...) semua assignment-nya kebuang.
    ERRMSG=""
    if ! assemble_identity "$row"; then
      log "baris brand '$brand' nggak valid:$ERRMSG — coba lagi"; continue
    fi
    ERRMSG=""
    if validate_lifecycle "$NOW"; then
      log "brand kepilih: '$brand' (dari $nbrands brand) -> $MARKETNAME"
      ok=1; break
    else
      log "riwayat '$brand/$MODEL' nggak nyambung:$ERRMSG — coba lagi"
    fi
  done

  if [ "$ok" -ne 1 ]; then
    log "belum dapat profil yang konsisten dalam $try percobaan — batal"
    return 1
  fi

  display_profile

  if [ "${AUTOPIF_NO_WRITE:-0}" = "1" ]; then
    log "AUTOPIF_NO_WRITE=1 — artifact nggak ditulis"
    return 0
  fi
  if printf '%s\n' "$IDENTITY_KV" > "$IDENTITY_ARTIFACT.tmp.$$" 2>/dev/null \
     && mv -f "$IDENTITY_ARTIFACT.tmp.$$" "$IDENTITY_ARTIFACT" 2>/dev/null; then
    chmod 0644 "$IDENTITY_ARTIFACT" 2>/dev/null
    log "beres — identity siap-pakai ditulis ke $IDENTITY_ARTIFACT"
  else
    rm -f "$IDENTITY_ARTIFACT.tmp.$$" 2>/dev/null
    log "gagal nulis artifact $IDENTITY_ARTIFACT"
  fi
  return 0
}

# --------------------------------------------------------------------------
# subcommand: fetch  (Pixel canary online -> persona.override)  [khusus Pixel]
# --------------------------------------------------------------------------

map_platform() {
  case "$1" in
    oriole|bluejay|raven)                 echo "gs101" ;;
    panther|cheetah|lynx)                 echo "gs201" ;;
    shiba|husky|akita)                    echo "zuma" ;;
    tokay|caiman|komodo|tegu|comet)       echo "zumapro" ;;
    frankel|blazer|mustang|rango)         echo "laguna" ;;
    *)                                    echo "" ;;
  esac
}

resolve_persona() {
  _idx="$1"
  _model=$(printf '%s\n'  "$MODEL_LIST"   | sed -n "${_idx}p")
  _device=$(printf '%s\n' "$PRODUCT_LIST" | sed -n "${_idx}p")
  [ -z "$_model" ] && return 1
  [ -z "$_device" ] && return 1
  _platform=$(map_platform "$_device")
  [ -z "$_platform" ] && return 1

  _station_url="https://content-flashstation-pa.googleapis.com/v1/builds?product=${_device}_beta&key=$FLASH_KEY"
  download "$_station_url" "station.json" "https://flash.android.com"
  [ -s "station.json" ] || { log "lewati $_model: nggak ada data build"; return 1; }

  if command -v tac >/dev/null 2>&1; then
    tac "station.json" | grep -m1 -A13 '"canary": true' > "canary.json" 2>/dev/null
  else
    grep -A20 '"canary": true' "station.json" 2>/dev/null | head -n 20 > "canary.json"
  fi
  [ -s "canary.json" ] || { log "lewati $_model: nggak ada build canary"; return 1; }

  _bid=$(grep 'releaseCandidateName' "canary.json" | cut -d\" -f4 | head -n1)
  _incr=$(grep 'buildId' "canary.json" | cut -d\" -f4 | head -n1)
  [ -z "$_bid" ] && { log "lewati $_model: nggak ada build id"; return 1; }
  [ -z "$_incr" ] && { log "lewati $_model: nggak ada incremental"; return 1; }

  # build canary "id" menyimpan bulan security-patch sbg YYYYMM tepat setelah
  # "canary-". Ambil ekor setelah "canary-" terakhir, ambil rentetan 6-digit
  # pertama, format YYYY-MM. Menolak input non-6-digit mencegah id rusak (yang
  # dulu membocorkan tanggal koma-nyasar spt "2026-08,-05") sampai ke freshen.
  _canary_tail=$(grep '"id"' "canary.json" | sed 's;.*canary-;;' | head -n1)
  _canary_ym=$(printf '%s' "$_canary_tail" | sed 's;^\([0-9]\{6\}\).*;\1;')
  case "$_canary_ym" in
    [0-9][0-9][0-9][0-9][0-9][0-9])
      _canary_id="$(printf '%s' "$_canary_ym" | cut -c1-4)-$(printf '%s' "$_canary_ym" | cut -c5-6)" ;;
    *) _canary_id="" ;;
  esac
  _spatch=""
  if [ -n "$_canary_id" ] && [ -s "secbull.html" ]; then
    _spatch=$(grep "<td>$_canary_id" "secbull.html" 2>/dev/null | sed 's;.*<td>\(.*\)</td>;\1;' | head -n1)
  fi
  [ -z "$_spatch" ] && [ -n "$_canary_id" ] && _spatch="${_canary_id}-05"
  # penjaga akhir: hanya terima security patch YYYY-MM-DD yang benar
  case "$_spatch" in
    [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]) : ;;
    *) log "lewati $_model: security patch '$_spatch' nggak kebaca"; return 1 ;;
  esac

  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$_model" "$_device" "$_device" "$_device" "$_platform" \
    "$CANARY_SDK" "$CANARY_RELEASE" "$_bid" "$_incr" "$_spatch"
  return 0
}

cmd_fetch() {
  rm -f "$OVERRIDE_FILE" 2>/dev/null

  DL=""
  if command -v curl >/dev/null 2>&1; then
    DL="curl"
  elif command -v wget >/dev/null 2>&1; then
    DL="wget"
  else
    log "nggak ada curl/wget — santai, freshen pakai pool bawaan (offline no-op)"
    return 0
  fi

  make_tmp || { log "nggak ada folder temp yang bisa ditulis — batal refresh"; return 0; }
  cleanup() { rm -rf "$TMP_DIR" 2>/dev/null; }
  trap cleanup EXIT
  cd "$TMP_DIR" 2>/dev/null || { log "nggak bisa masuk folder temp — batal"; return 0; }

  download "https://developer.android.com/about/versions" "versions.html"
  LATEST_URL=$(grep -o 'https://developer.android.com/about/versions/.*[0-9]"' "versions.html" 2>/dev/null | sed 's;.*/\([0-9][0-9]*\)"$;\1 &;' | sort -rn | cut -d' ' -f2- | cut -d\" -f1 | head -n1)
  [ -z "$LATEST_URL" ] && { log "nggak bisa resolve URL versi terbaru — pakai pool bawaan"; return 0; }
  download "$LATEST_URL" "latest.html"

  FI_PATH=$(grep -o 'href=".*download.*"' "latest.html" 2>/dev/null | grep 'qpr' | cut -d\" -f2 | head -n1)
  [ -z "$FI_PATH" ] && { log "nggak nemu link factory-image — pakai pool bawaan"; return 0; }
  download "https://developer.android.com$FI_PATH" "fi.html"

  MODEL_LIST=$(grep -A1 'tr id=' "fi.html" 2>/dev/null | grep 'td' | sed 's;.*<td>\(.*\)</td>.*;\1;')
  PRODUCT_LIST=$(grep 'tr id=' "fi.html" 2>/dev/null | sed 's;.*<tr id="\(.*\)">.*;\1;')

  count_model=$(printf '%s\n' "$MODEL_LIST"   | grep -c .)
  count_prod=$(printf '%s\n'  "$PRODUCT_LIST" | grep -c .)
  if [ "$count_model" -eq 0 ] || [ "$count_model" -ne "$count_prod" ]; then
    log "parse tabel device gagal (models=$count_model products=$count_prod) — pakai pool bawaan"
    return 0
  fi

  known=""
  i=1
  while [ "$i" -le "$count_model" ]; do
    device=$(printf '%s\n' "$PRODUCT_LIST" | sed -n "${i}p")
    [ -n "$device" ] && [ -n "$(map_platform "$device")" ] && known="$known $i"
    i=$((i + 1))
  done

  set -- $known
  kn=$#
  if [ "$kn" -eq 0 ]; then
    log "nggak ada model SoC-dikenal di daftar Google — pakai pool bawaan"
    return 0
  fi

  download "https://flash.android.com" "flash.html"
  FLASH_KEY=$(grep -o '<body data-client-config=.*' "flash.html" 2>/dev/null | cut -d\; -f2 | cut -d\& -f1)
  [ -z "$FLASH_KEY" ] && { log "nggak dapat API key flash — pakai pool bawaan"; return 0; }

  download "https://source.android.com/docs/security/bulletin/pixel" "secbull.html"

  start=$(rand_below "$kn")
  persona_line=""
  try=0
  row=""
  while [ "$try" -lt "$kn" ] && [ "$try" -lt "$MAX_TRY" ]; do
    off=$(( (start + try) % kn ))
    try=$((try + 1))
    eval "row=\${$((off + 1))}"
    persona_line=$(resolve_persona "$row")
    [ -n "$persona_line" ] && break
  done

  if [ -z "$persona_line" ]; then
    log "nggak ada persona canary dalam $try percobaan — pakai pool bawaan"
    return 0
  fi

  if printf '%s\n' "$persona_line" > "$OVERRIDE_FILE.tmp.$$" 2>/dev/null \
     && mv -f "$OVERRIDE_FILE.tmp.$$" "$OVERRIDE_FILE" 2>/dev/null; then
    chmod 0644 "$OVERRIDE_FILE" 2>/dev/null
    _m=$(printf '%s' "$persona_line" | cut -f1)
    _d=$(printf '%s' "$persona_line" | cut -f2)
    _b=$(printf '%s' "$persona_line" | cut -f8)
    log "dapat persona canary acak: $_m ($_d) build $_b — freshen langsung memakainya"
  else
    rm -f "$OVERRIDE_FILE.tmp.$$" 2>/dev/null
    log "nggak bisa nulis persona override — pakai pool bawaan"
  fi

  return 0
}

# --------------------------------------------------------------------------
# dispatch  (device = default sekarang, biar acak multibrand yang jalan)
# --------------------------------------------------------------------------
case "${1:-device}" in
  device|gen|multibrand|profile|"") cmd_device ;;
  fetch)                            cmd_fetch ;;
  *) log "subcommand '$1' nggak dikenal (pakai: device | fetch)"; exit 2 ;;
esac
exit $?
