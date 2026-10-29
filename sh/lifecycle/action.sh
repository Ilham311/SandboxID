#!/system/bin/sh

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

[ -r "$MODDIR/helpers.sh" ] && . "$MODDIR/helpers.sh" 2>/dev/null
command -v _fw_run    >/dev/null 2>&1 || _fw_run()    { "$@" </dev/null >/dev/null 2>&1; }
command -v force_stop >/dev/null 2>&1 || force_stop() { am force-stop --user 0 "$1" </dev/null >/dev/null 2>&1; }
command -v identity_get >/dev/null 2>&1 || identity_get() {
    awk -F= -v k="$1" '$1==k { sub(/^[^=]*=/, ""); print; exit }' "$IDENTITY" 2>/dev/null
}
# Fallbacks so a missing helpers.sh degrades instead of aborting the reset: no
# permission list means no grant pass (the app asks for permissions itself), and
# no launch means the user opens the app by hand, which is the old behaviour.
command -v pkg_granted_permissions  >/dev/null 2>&1 || pkg_granted_permissions()  { return 1; }
command -v pkg_restore_permissions  >/dev/null 2>&1 || pkg_restore_permissions()  { printf '0\n'; return 1; }
command -v pkg_grant_persona_permissions >/dev/null 2>&1 || pkg_grant_persona_permissions() { printf '0\n'; return 1; }
command -v pkg_persona_permissions  >/dev/null 2>&1 || pkg_persona_permissions()  { return 1; }
command -v pkg_launch               >/dev/null 2>&1 || pkg_launch()               { return 1; }

if command -v sbx_bin >/dev/null 2>&1; then
    BIN="$(sbx_bin)"
else
    [ -x "$BIN" ] || BIN=""
fi
[ -n "$BIN" ] || printf '%s\n' "Peringatan: binary native tidak ditemukan di $MODDIR/bin/ (sandboxid, sandboxid-arm64/-arm/-x86_64/-x). Re-flash module untuk memperbaiki."

tee2() { tee -a "$LOGFILE" "$ACTION_LOG"; }
tee_action() { tee -a "$ACTION_LOG"; }
say()  { printf '%s\n' "$*" | tee2; }

{
  echo ""
  echo "=== $(date '+%F %T') action.sh (moddir=$MODDIR) ==="
} >> "$ACTION_LOG" 2>/dev/null

RC=0
RC_ROT=0
APPLIED=""

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

if [ -x "$BIN" ] && [ -s "$DEVICE_ID" ]; then
    [ -f "$IDENTITY" ] && cp -f "$IDENTITY" "$MODDIR/identity.prop.bak" 2>/dev/null
    if cp -f "$DEVICE_ID" "$IDENTITY" 2>/dev/null; then
        chmod 0644 "$IDENTITY" 2>/dev/null
        "$BIN" unlock >/dev/null 2>&1 || true
        say ""
        say "==> Menerapkan identitas (seed; spoofing per-target in-process)"
        APPLY_OUT="$MODDIR/debug/.apply.$$"
        "$BIN" seed </dev/null >"$APPLY_OUT" 2>&1; RC=$?
        tee2 < "$APPLY_OUT"; rm -f "$APPLY_OUT" 2>/dev/null
        "$BIN" lock >/dev/null 2>&1 || true
        [ "$RC" = 0 ] && APPLIED="multibrand"
    else
        say "Gagal menyalin hasil acak ke identity.prop — pakai metode lama."
    fi
fi

if [ -z "$APPLIED" ]; then
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

if [ "$APPLIED" = "multibrand" ]; then
    say ""
    say "==> Mereset aplikasi target (agar membaca identitas baru)"
    if [ -r "$MODDIR/target.txt" ] && command -v pm >/dev/null 2>&1; then
        _wiped=0
        LAUNCH_LIST=""
        while IFS= read -r _line || [ -n "$_line" ]; do
            _line=${_line%%#*}
            _line=$(printf '%s' "$_line" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
            [ -n "$_line" ] || continue
            force_stop "$_line" >/dev/null 2>&1

            # Capture the granted permissions BEFORE the wipe. `pm clear`
            # revokes them all, and the next launch then opens the system
            # permission screen instead of the app's splash — the app never
            # reaches the code that registers, so a reset that reports success
            # would mint nothing. Read from the device rather than hard-coded
            # so it tracks the app's current permission set.
            _perms="$(pkg_granted_permissions "$_line" 2>/dev/null)"
            _nperm=$(printf '%s\n' "$_perms" | awk 'NF { n++ } END { print n + 0 }')

            if _fw_run pm clear --user 0 "$_line"; then
                say "   - $_line — direset"
                # The package manager is still finishing the clear; a grant
                # issued in the same breath is dropped.
                sleep 1
                if [ "$_nperm" -gt 0 ]; then
                    _restored=$(pkg_restore_permissions "$_line" "$_perms" 2>/dev/null)
                    case "$_restored" in ''|*[!0-9]*) _restored=0 ;; esac
                    say "     izin aktif kembali: $_restored dari $_nperm"
                fi
                # Restoring only puts back what was granted BEFORE the wipe. A
                # permission the user never granted stays denied, and an app
                # that gates an identifier read on it takes a different code
                # path - Grab records the ANDROID_ID in its IMEI field when
                # READ_PHONE_STATE is denied. Grant the persona's baseline set
                # explicitly so the app runs the path a retail device would.
                _persona_n=$(pkg_grant_persona_permissions "$_line" 2>/dev/null)
                case "$_persona_n" in ''|*[!0-9]*) _persona_n=0 ;; esac
                _persona_want=$(pkg_persona_permissions 2>/dev/null | awk 'NF { n++ } END { print n + 0 }')
                if [ "$_persona_n" -gt 0 ]; then
                    say "     izin baseline persona: $_persona_n dari $_persona_want"
                fi
                # Launched at the very end, after rotate_ids.sh has seeded the
                # new AppLog files: starting the app first would have it read
                # the previous identity and cache it.
                LAUNCH_LIST="${LAUNCH_LIST}${_line}
"
            else
                say "   - $_line — dilewati (belum terpasang?)"
            fi
            _wiped=$((_wiped + 1))
        done < "$MODDIR/target.txt"
        [ "$_wiped" = 0 ] && say "   target.txt kosong — tidak ada aplikasi yang direset (aman, sesuai desain)."
    else
        say "   target.txt kosong / pm tidak ada — dilewati."
    fi
fi

if [ -r "$ROTATE" ]; then
    say ""
    say "==> Rotasi ID lain (SSAID, WiFi/BT MAC, nama, boot count, AppLog)"
    ROT_OUT="$MODDIR/debug/.rotate.$$"
    MODDIR="$MODDIR" LOGFILE="$LOGFILE" sh "$ROTATE" all </dev/null >"$ROT_OUT" 2>&1; RC_ROT=$?
    tee_action < "$ROT_OUT"; rm -f "$ROT_OUT" 2>/dev/null
else
    say "==> Rotasi ID dilewati (rotate_ids.sh tidak ada)."
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
    say "  ANDROID ID  : $_aid"
    say ""
    say "Selesai. Buka ulang aplikasi target — sekarang membaca identitas perangkat di atas."
elif [ "$APPLIED" = "freshen" ]; then
    say "OK - persona cadangan Pixel aktif — lihat detail MODEL di atas."
else
    say "Gagal menerapkan identitas (rc=$RC). Cek pesan di atas atau tab Log."
fi

[ "$RC_ROT" != 0 ] && \
    say "  (catatan: rotasi ID rc=$RC_ROT — sebagian ID mungkin belum berganti; cek tab Log)"

# Launch last, after the AppLog files have been seeded. Starting the app before
# rotate_ids.sh runs would have it read the previous identity from disk and cache
# it for the whole process, and hooks are installed once per process — so the
# reset would be visible on disk and invisible to the app.
if [ -n "${LAUNCH_LIST:-}" ]; then
    say ""
    say "==> Membuka aplikasi target"
    _old_ifs="$IFS"
    IFS='
'
    for _line in $LAUNCH_LIST; do
        [ -n "$_line" ] || continue
        if pkg_launch "$_line"; then
            say "   - $_line — dibuka"
        else
            say "   - $_line — gagal dibuka, buka manual"
        fi
    done
    IFS="$_old_ifs"
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
