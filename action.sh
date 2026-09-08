#!/system/bin/sh

MODDIR="${0%/*}"
BIN="$MODDIR/bin/sandboxid"
AUTOPIF="$MODDIR/autopif.sh"
DEVICE_ID="$MODDIR/device.identity"
IDENTITY="$MODDIR/identity.prop"

LOGFILE="${LOGFILE:-/cache/sandboxid-boot.log}"
[ -w "$(dirname "$LOGFILE")" ] || LOGFILE="/data/local/tmp/sandboxid-boot.log"
touch "$LOGFILE" 2>/dev/null

ACTION_LOG="$MODDIR/debug/action.log"
mkdir -p "$MODDIR/debug" 2>/dev/null

[ -r "$MODDIR/helpers.sh" ] && . "$MODDIR/helpers.sh" 2>/dev/null
command -v identity_get >/dev/null 2>&1 || identity_get() {
    awk -F= -v k="$1" '$1==k { sub(/^[^=]*=/, ""); print; exit }' "$IDENTITY" 2>/dev/null
}

if command -v sbx_bin >/dev/null 2>&1; then
    BIN="$(sbx_bin)"
else
    [ -x "$BIN" ] || BIN=""
fi
[ -n "$BIN" ] || printf '%s\n' "Peringatan: binary native tidak ditemukan di $MODDIR/bin/ (sandboxid, sandboxid-arm64/-arm/-x86_64/-x). Re-flash module untuk memperbaiki."

tee2() { tee -a "$LOGFILE" "$ACTION_LOG"; }
say()  { printf '%s\n' "$*" | tee2; }

{
  echo ""
  echo "=== $(date '+%F %T') action.sh (moddir=$MODDIR) ==="
} >> "$ACTION_LOG" 2>/dev/null

RC=0
APPLIED=""
OEM_ARTIFACT=0

say ""
say "SandboxID — membuat identitas perangkat baru"
say ""

rm -f "$DEVICE_ID" 2>/dev/null
if [ -f "$AUTOPIF" ] && [ -f "$MODDIR/devices.tsv" ]; then
    say "==> Mengacak perangkat"
    MODDIR="$MODDIR" sh "$AUTOPIF" device 2>&1 | tee2
else
    say "==> Pengacakan dilewati (autopif.sh / devices.tsv tidak ada) — pakai metode lama."
fi

if [ -s "$DEVICE_ID" ]; then
    OEM_ARTIFACT=1
    if [ -x "$BIN" ]; then
        "$BIN" unlock >/dev/null 2>&1 || true
        IMPORT_OUT="$MODDIR/debug/.import.$$"
        "$BIN" import "$DEVICE_ID" </dev/null >"$IMPORT_OUT" 2>&1; RC=$?
        tee2 < "$IMPORT_OUT"; rm -f "$IMPORT_OUT" 2>/dev/null
        "$BIN" lock >/dev/null 2>&1 || true
        [ "$RC" = 0 ] && APPLIED="multibrand"
    else
        say "Gagal: artifact OEM dibuat, tetapi binary native tidak bisa dijalankan."
        RC=127
    fi
fi

if [ "$OEM_ARTIFACT" = 0 ]; then
    if [ -x "$BIN" ]; then
        "$BIN" unlock >/dev/null 2>&1 || true
        say ""
        say "==> Metode cadangan: freshen (persona Pixel bawaan)"
        FR_OUT="$MODDIR/debug/.freshen.$$"
        "$BIN" freshen </dev/null >"$FR_OUT" 2>&1; RC=$?
        tee2 < "$FR_OUT"; rm -f "$FR_OUT" 2>/dev/null
        "$BIN" lock >/dev/null 2>&1 || true
        [ "$RC" = 0 ] && APPLIED="freshen"
    else
        say "Gagal: binary native tidak bisa dijalankan (BIN='$BIN')."
        say "       Cek isi $MODDIR/bin/ — harus ada sandboxid atau sandboxid-{arm64,arm,x86_64,x}."
        say "       Re-flash module zip untuk memperbaiki pemasangan."
        RC=127
    fi
fi

if [ -r "$MODDIR/helpers.sh" ] && [ -r "$MODDIR/target.txt" ] && \
   grep -qE '^[[:space:]]*[^[:space:]#]' "$MODDIR/target.txt" 2>/dev/null; then
    _applog_summary=""
    while IFS= read -r _line || [ -n "$_line" ]; do
        _line=${_line%%#*}
        _line=$(printf '%s' "$_line" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
        [ -n "$_line" ] || continue
        _probe=$(applog_probe "$_line" 2>/dev/null)
        [ -n "$_probe" ] && _applog_summary="${_applog_summary}${_probe}\n"
    done < "$MODDIR/target.txt"
    if [ -n "$_applog_summary" ]; then
        say ""
        say "==> Status AppLog per aplikasi target:"
        printf "%b" "$_applog_summary" | while IFS=' ' read -r _p _n _st; do
            [ -n "$_p" ] || continue
            case "$_st" in
                active) say "   - $_p  ($_n file, cache SDK ada — nilai di-spoof in-process)" ;;
                fresh)  say "   - $_p  ($_n file, cache bersih menunggu app dibuka)" ;;
                absent) say "   - $_p  (tidak terpasang)" ;;
                *)      say "   - $_p  ($_n file, $_st)" ;;
            esac
        done
    fi
fi

say ""
if [ "$APPLIED" = "multibrand" ]; then
    _brand=$(identity_get BRAND);      _mkt=$(identity_get MARKETNAME)
    _model=$(identity_get MODEL);      _dev=$(identity_get DEVICE)
    _rel=$(identity_get RELEASE);      _sdk=$(identity_get SDK_INT)
    _fp=$(identity_get FINGERPRINT);   _bc=$(identity_get BOOT_COUNT)
    _up=$(identity_get UPTIME_HUMAN);  _ser=$(identity_get SERIAL)
    _aid=$(identity_get ANDROID_ID)
    say "OK - persona baru aktif"
    say "  BRAND       : $_brand"
    say "  MODEL       : $_mkt ($_model)"
    say "  DEVICE      : $_dev"
    say "  RELEASE     : Android $_rel (SDK $_sdk)"
    say "  FINGERPRINT : $_fp"
    say "  BOOT COUNT  : $_bc"
    say "  UPTIME      : $_up"
    say "  SERIAL      : $_ser"
    say "  ENTROPY ID  : $_aid (metadata profil; bukan hasil API SSAID)"
    say ""
    say "Selesai. Mulai ulang hanya proses aplikasi target secara manual, lalu buka lagi. Data dan izin aplikasi tidak dihapus; SSAID, rotasi agregat, dan properti global tidak disentuh."
elif [ "$APPLIED" = "freshen" ]; then
    say "OK - persona cadangan Pixel aktif — lihat detail MODEL di atas."
    say "Mulai ulang hanya proses aplikasi target secara manual. Data/izin aplikasi, SSAID, rotasi agregat, dan properti global tidak disentuh."
else
    say "Gagal menerapkan identitas (rc=$RC). Cek pesan di atas atau tab Log."
fi

if [ -f "$MODDIR/debug_variant" ] && [ -d "$MODDIR/debug" ]; then
    LATEST=$(ls -1t "$MODDIR/debug"/session-*.log 2>/dev/null | head -1)
    if [ -n "$LATEST" ]; then
        OUTDIR="$MODDIR/debug/report"
        mkdir -p "$OUTDIR" 2>/dev/null
        chmod 0700 "$OUTDIR" 2>/dev/null
        TS=$(date +%Y%m%d-%H%M%S)

        SUMMARY="$OUTDIR/summary-$TS.txt"
        [ -f "$MODDIR/summarize.sh" ] && sh "$MODDIR/summarize.sh" "$LATEST" "$SUMMARY" 2>/dev/null

        [ -f "$MODDIR/debug/crashes.log" ] && \
            cp "$MODDIR/debug/crashes.log" "$OUTDIR/crashes-$TS.log" 2>/dev/null

        for pattern in "summary-*.txt" "crashes-*.log"; do
            ls -1t $OUTDIR/$pattern 2>/dev/null | tail -n +11 | xargs rm -f 2>/dev/null
        done

        say ""
        say "Artefak debug (khusus root): $OUTDIR/"
        [ -f "$SUMMARY" ] && say "   - ringkasan  $(basename "$SUMMARY")  ($(du -h "$SUMMARY" | cut -f1))"
        [ -f "$OUTDIR/crashes-$TS.log" ] && say "   - crashes    crashes-$TS.log  ($(du -h "$OUTDIR/crashes-$TS.log" | cut -f1))"
        say "   Log lengkap ada di tab Log WebUI, atau file session-*.log di folder debug/."
    fi
fi

exit "$RC"
