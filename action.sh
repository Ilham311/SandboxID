#!/system/bin/sh

MODDIR="${0%/*}"
BIN="$MODDIR/bin/ternak-tt"
ROTATE="$MODDIR/rotate_ids.sh"
LOGFILE="/cache/ternak-tt-boot.log"
[ -w "$(dirname "$LOGFILE")" ] || LOGFILE="/data/local/tmp/ternak-tt-boot.log"
touch "$LOGFILE" 2>/dev/null

ACTION_LOG="$MODDIR/debug/action.log"
mkdir -p "$MODDIR/debug" 2>/dev/null
{
  echo ""
  echo "=== $(date '+%F %T') action.sh (moddir=$MODDIR) ==="
} >> "$ACTION_LOG" 2>/dev/null

RC_FRESHEN=0
RC_ROTATE=0

if [ -x "$BIN" ]; then
    echo "[Ternak TT] freshen (step 1/2)..."
    "$BIN" unlock >/dev/null 2>&1 || true
    # M5: PIPESTATUS tidak reliabel di busybox/toybox ash. Jalankan ke file,
    # tangkap exit code langsung ($?), lalu baru tee ke log.
    FRESHEN_OUT="$MODDIR/debug/.freshen.$$"
    "$BIN" freshen > "$FRESHEN_OUT" 2>&1; RC_FRESHEN=$?
    tee -a "$LOGFILE" "$ACTION_LOG" < "$FRESHEN_OUT"
    rm -f "$FRESHEN_OUT" 2>/dev/null
    "$BIN" lock >/dev/null 2>&1 || true
    echo "[Ternak TT] auto-locked after freshen" | tee -a "$LOGFILE" "$ACTION_LOG"
else
    echo "[Ternak TT] ERROR: $BIN not executable" | tee -a "$LOGFILE"
    RC_FRESHEN=127
fi

if [ -r "$ROTATE" ]; then
    echo "[Ternak TT] rotate_ids all (step 2/2)..."
    ROTATE_OUT="$MODDIR/debug/.rotate.$$"
    MODDIR="$MODDIR" LOGFILE="$LOGFILE" sh "$ROTATE" all > "$ROTATE_OUT" 2>&1; RC_ROTATE=$?
    tee -a "$LOGFILE" "$ACTION_LOG" < "$ROTATE_OUT"
    rm -f "$ROTATE_OUT" 2>/dev/null
else
    echo "[Ternak TT] WARN: rotate_ids.sh missing - skipping shell-layer rotation" | tee -a "$LOGFILE"
fi

echo ""
echo "[Ternak TT] freshen rc=$RC_FRESHEN, rotate rc=$RC_ROTATE"
if [ "$RC_ROTATE" != "0" ] && [ "$RC_ROTATE" != "1" ]; then
    echo "[Ternak TT] see $LOGFILE for details"
fi

if [ -f "$MODDIR/debug_variant" ] && [ -d "$MODDIR/debug" ]; then
    LATEST=$(ls -1t "$MODDIR/debug"/session-*.log 2>/dev/null | head -1)
    if [ -n "$LATEST" ]; then
        # H4: simpan artifact di dalam MODDIR (root-only /data/adb 0700), BUKAN
        # /sdcard/Download yang world-readable — log memuat GAID/MAC/serial.
        # Ambil lewat WebUI (root bridge) atau root file manager.
        OUTDIR="$MODDIR/debug/report"
        mkdir -p "$OUTDIR" 2>/dev/null
        chmod 0700 "$OUTDIR" 2>/dev/null
        TS=$(date +%Y%m%d-%H%M%S)

        SUMMARY="$OUTDIR/summary-$TS.txt"
        if [ -x "$MODDIR/summarize.sh" ] || [ -f "$MODDIR/summarize.sh" ]; then
            sh "$MODDIR/summarize.sh" "$LATEST" "$SUMMARY" 2>/dev/null
        fi

        [ -f "$MODDIR/debug/crashes.log" ] && \
            cp "$MODDIR/debug/crashes.log" "$OUTDIR/crashes-$TS.log" 2>/dev/null

        RAW_GZ="$OUTDIR/session-$TS.log.gz"
        gzip -c "$LATEST" > "$RAW_GZ" 2>/dev/null

        for pattern in "summary-*.txt" "crashes-*.log" "session-*.log.gz"; do
            ls -1t $OUTDIR/$pattern 2>/dev/null | tail -n +11 | xargs rm -f 2>/dev/null
        done

        echo ""
        echo "[Ternak TT debug artifacts] (root-only)"
        echo "  dir: $OUTDIR/"
        [ -f "$SUMMARY" ] && echo "  ok summary  $(basename $SUMMARY)  ($(du -h $SUMMARY | cut -f1))"
        [ -f "$OUTDIR/crashes-$TS.log" ] && echo "  ok crashes  crashes-$TS.log  ($(du -h $OUTDIR/crashes-$TS.log | cut -f1))"
        [ -f "$RAW_GZ" ] && echo "  ok raw.gz   $(basename $RAW_GZ)  ($(du -h $RAW_GZ | cut -f1))"
        echo ""
        echo "View via WebUI Log tab, or pull with a root file manager."
    fi
fi

exit $RC_FRESHEN
