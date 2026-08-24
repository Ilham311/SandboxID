#!/system/bin/sh
# SandboxID — tombol Action (WebUI / KernelSU).
#
# Alurnya simpel:
#   ① undi 1 device acak dari SEMUA brand (peluang tiap brand sama rata)
#   ② pasang identitasnya apa adanya  -> sandboxid apply-boot
#   ③ reset app target biar baca identitas baru
#   ④ rotasi ID lain: SSAID · GAID · WiFi/BT MAC · nama · boot count
#
# Semua opt-in: cuma jalan kalau kamu yang pencet, dan cuma nyentuh app yang
# kamu daftarin di target.txt. Kalau undian multibrand gagal (mis. devices.tsv
# hilang), otomatis mundur ke cara lama (freshen Pixel) biar tombol tetap guna.

MODDIR="${0%/*}"
BIN="$MODDIR/bin/sandboxid"
ROTATE="$MODDIR/rotate_ids.sh"
AUTOPIF="$MODDIR/autopif.sh"
DEVICE_ID="$MODDIR/device.identity"
IDENTITY="$MODDIR/identity.prop"

LOGFILE="/cache/sandboxid-boot.log"
[ -w "$(dirname "$LOGFILE")" ] || LOGFILE="/data/local/tmp/sandboxid-boot.log"
touch "$LOGFILE" 2>/dev/null

ACTION_LOG="$MODDIR/debug/action.log"
mkdir -p "$MODDIR/debug" 2>/dev/null

# helpers.sh -> _fw_run (std-FD /dev/null, anti binder-fail), force_stop,
# identity_get. Kalau nggak ada, pasang versi minimal biar action.sh mandiri.
[ -r "$MODDIR/helpers.sh" ] && . "$MODDIR/helpers.sh" 2>/dev/null
command -v _fw_run    >/dev/null 2>&1 || _fw_run()    { "$@" </dev/null >/dev/null 2>&1; }
command -v force_stop >/dev/null 2>&1 || force_stop() { am force-stop --user 0 "$1" </dev/null >/dev/null 2>&1; }
command -v identity_get >/dev/null 2>&1 || identity_get() {
    awk -F= -v k="$1" '$1==k { sub(/^[^=]*=/, ""); print; exit }' "$IDENTITY" 2>/dev/null
}

# tee ke layar + log boot + log action
tee2() { tee -a "$LOGFILE" "$ACTION_LOG"; }
say()  { printf '%s\n' "$*" | tee2; }

{
  echo ""
  echo "=== $(date '+%F %T') action.sh (moddir=$MODDIR) ==="
} >> "$ACTION_LOG" 2>/dev/null

RC=0
RC_ROT=0
APPLIED=""

say ""
say "SandboxID — bikin identitas device baru"
say "Diundi acak dari semua brand (Pixel, Samsung, Xiaomi, POCO, OPPO, vivo, Redmi, Infinix), peluang tiap brand sama rata."
say "Versi Android-nya dikunci sesuai perangkatmu biar app target nggak error."
say ""

# ── ① undi 1 device multibrand -> device.identity ───────────────────────────
rm -f "$DEVICE_ID" 2>/dev/null   # never re-apply a stale draw / let fallback work
if [ -f "$AUTOPIF" ] && [ -f "$MODDIR/devices.tsv" ]; then
    say "① Ngundi device…"
    MODDIR="$MODDIR" sh "$AUTOPIF" device 2>&1 | tee2
else
    say "① Undian dilewat (autopif.sh / devices.tsv nggak ada) — pakai cara lama."
fi

# ── ② pasang identitas hasil undian apa adanya (verbatim, tanpa Googlefikasi) ─
if [ -x "$BIN" ] && [ -s "$DEVICE_ID" ]; then
    [ -f "$IDENTITY" ] && cp -f "$IDENTITY" "$MODDIR/identity.prop.bak" 2>/dev/null
    if cp -f "$DEVICE_ID" "$IDENTITY" 2>/dev/null; then
        chmod 0644 "$IDENTITY" 2>/dev/null
        "$BIN" unlock >/dev/null 2>&1 || true
        say ""
        say "② Memasang identitas ke sistem (apply-boot)…"
        APPLY_OUT="$MODDIR/debug/.apply.$$"
        "$BIN" apply-boot </dev/null >"$APPLY_OUT" 2>&1; RC=$?
        tee2 < "$APPLY_OUT"; rm -f "$APPLY_OUT" 2>/dev/null
        "$BIN" lock >/dev/null 2>&1 || true
        [ "$RC" = 0 ] && APPLIED="multibrand"
    else
        say "② Gagal menyalin hasil undian ke identity.prop — pakai cara lama."
    fi
fi

# ── ②b cadangan: kalau undian/apply gagal, pakai freshen (Pixel) ────────────
if [ -z "$APPLIED" ]; then
    if [ -x "$BIN" ]; then
        "$BIN" unlock >/dev/null 2>&1 || true
        say ""
        say "② Cadangan: freshen (persona Pixel bawaan)…"
        FR_OUT="$MODDIR/debug/.freshen.$$"
        "$BIN" freshen </dev/null >"$FR_OUT" 2>&1; RC=$?
        tee2 < "$FR_OUT"; rm -f "$FR_OUT" 2>/dev/null
        "$BIN" lock >/dev/null 2>&1 || true
        [ "$RC" = 0 ] && APPLIED="freshen"
    else
        say "Gagal: $BIN nggak bisa dijalankan — identitas native nggak dipasang."
        RC=127
    fi
fi

# ── ③ reset app target (pm clear + force-stop) ──────────────────────────────
# apply-boot nggak nge-reset app (cuma freshen yang wipe). Jadi di jalur
# multibrand kita reset sendiri app di target.txt: biar mereka baca identitas
# baru dari nol & buang cache/ID lama. pm/am dijalanin dgn std-FD /dev/null
# (kalau nggak, system_server nolak dgn FAILED_TRANSACTION).
if [ "$APPLIED" = "multibrand" ]; then
    say ""
    say "③ Reset app target (biar baca identitas baru)…"
    if [ -r "$MODDIR/target.txt" ] && command -v pm >/dev/null 2>&1; then
        _wiped=0
        while IFS= read -r _line || [ -n "$_line" ]; do
            _line=${_line%%#*}
            _line=$(printf '%s' "$_line" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
            [ -n "$_line" ] || continue
            force_stop "$_line" >/dev/null 2>&1
            if _fw_run pm clear --user 0 "$_line"; then
                say "   - $_line — direset"
            else
                say "   - $_line — dilewat (belum terpasang?)"
            fi
            _wiped=$((_wiped + 1))
        done < "$MODDIR/target.txt"
        [ "$_wiped" = 0 ] && say "   target.txt kosong — nggak ada app yang direset (aman, sesuai desain)."
    else
        say "   target.txt kosong / pm nggak ada — dilewat."
    fi
fi

# ── ④ rotasi ID lain (SSAID · GAID · MAC · nama · boot count) ────────────────
if [ -r "$ROTATE" ]; then
    say ""
    say "④ Rotasi ID lain (SSAID · GAID · WiFi/BT MAC · nama · boot count)…"
    ROT_OUT="$MODDIR/debug/.rotate.$$"
    MODDIR="$MODDIR" LOGFILE="$LOGFILE" sh "$ROTATE" all </dev/null >"$ROT_OUT" 2>&1; RC_ROT=$?
    tee2 < "$ROT_OUT"; rm -f "$ROT_OUT" 2>/dev/null
else
    say "④ rotate_ids.sh nggak ada — rotasi ID dilewat."
fi

# ── ⑤ ringkasan: enak dibaca + bisa diparse WebUI ───────────────────────────
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
    say "  ANDROID ID  : $_aid"
    say ""
    say "Selesai — buka lagi app targetmu, sekarang dia lihat device di atas."
elif [ "$APPLIED" = "freshen" ]; then
    say "OK - persona (cadangan Pixel) aktif. Lihat detail MODEL di atas."
else
    say "Gagal pasang identitas (rc=$RC). Cek pesan di atas atau tab Log."
fi

[ "$RC_ROT" != 0 ] && [ "$RC_ROT" != 1 ] && \
    say "  (catatan: rotasi ID rc=$RC_ROT — sebagian ID mungkin belum ganti; cek tab Log)"

# ── ⑥ artefak debug (root-only): ringkasan + crashes; TANPA .log.gz ─────────
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

        # Simpan 10 terbaru per jenis. Log mentah TIDAK di-gzip lagi — cukup buka
        # session-*.log langsung lewat tab Log (lebih enak dibaca, nggak perlu unzip).
        for pattern in "summary-*.txt" "crashes-*.log"; do
            ls -1t $OUTDIR/$pattern 2>/dev/null | tail -n +11 | xargs rm -f 2>/dev/null
        done

        say ""
        say "Artefak debug (root-only): $OUTDIR/"
        [ -f "$SUMMARY" ] && say "   • ringkasan  $(basename "$SUMMARY")  ($(du -h "$SUMMARY" | cut -f1))"
        [ -f "$OUTDIR/crashes-$TS.log" ] && say "   • crashes    crashes-$TS.log  ($(du -h "$OUTDIR/crashes-$TS.log" | cut -f1))"
        say "   Log lengkap: buka tab Log di WebUI, atau file session-*.log di folder debug/."
    fi
fi

exit "$RC"
