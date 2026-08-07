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
    "$BIN" freshen 2>&1 | tee -a "$LOGFILE" "$ACTION_LOG"
    RC_FRESHEN=${PIPESTATUS:-$?}
    "$BIN" lock >/dev/null 2>&1 || true
    echo "[Ternak TT] auto-locked after freshen" | tee -a "$LOGFILE" "$ACTION_LOG"
else
    echo "[Ternak TT] ERROR: $BIN not executable" | tee -a "$LOGFILE"
    RC_FRESHEN=127
fi

if [ -r "$ROTATE" ]; then
    echo "[Ternak TT] rotate_ids all (step 2/2)..."
    MODDIR="$MODDIR" LOGFILE="$LOGFILE" sh "$ROTATE" all 2>&1 | tee -a "$LOGFILE" "$ACTION_LOG"
    RC_ROTATE=${PIPESTATUS:-$?}
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
        OUTDIR="/sdcard/Download/ternak-tt-logs"
        mkdir -p "$OUTDIR" 2>/dev/null
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
        echo "[Ternak TT debug artifacts]"
        echo "  target: $OUTDIR/"
        [ -f "$SUMMARY" ] && echo "  ok summary  $(basename $SUMMARY)  ($(du -h $SUMMARY | cut -f1))  <- SHARE THIS"
        [ -f "$OUTDIR/crashes-$TS.log" ] && echo "  ok crashes  crashes-$TS.log  ($(du -h $OUTDIR/crashes-$TS.log | cut -f1))"
        [ -f "$RAW_GZ" ] && echo "  ok raw.gz   $(basename $RAW_GZ)  ($(du -h $RAW_GZ | cut -f1))  <- full log if needed"
        echo ""
        echo "Share summary-*.txt first - small enough to paste."
    fi
fi

exit $RC_FRESHEN
