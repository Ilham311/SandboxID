#!/system/bin/sh
MODDIR="${0%/*}"

"$MODDIR/bin/ternak-tt" freshen
RC=$?

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

        if [ -f "$MODDIR/debug/crashes.log" ]; then
            cp "$MODDIR/debug/crashes.log" "$OUTDIR/crashes-$TS.log" 2>/dev/null
        fi

        RAW_GZ="$OUTDIR/session-$TS.log.gz"
        gzip -c "$LATEST" > "$RAW_GZ" 2>/dev/null

        for pattern in "summary-*.txt" "crashes-*.log" "session-*.log.gz"; do
            ls -1t $OUTDIR/$pattern 2>/dev/null | tail -n +11 | xargs rm -f 2>/dev/null
        done

        echo ""
        echo "[Ternak TT debug artifacts]"
        echo "  target: $OUTDIR/"
        [ -f "$SUMMARY" ] && echo "  ✓ summary  $(basename $SUMMARY)  ($(du -h $SUMMARY | cut -f1))  ← SHARE THIS"
        [ -f "$OUTDIR/crashes-$TS.log" ] && echo "  ✓ crashes  crashes-$TS.log  ($(du -h $OUTDIR/crashes-$TS.log | cut -f1))"
        [ -f "$RAW_GZ" ] && echo "  ✓ raw.gz   $(basename $RAW_GZ)  ($(du -h $RAW_GZ | cut -f1))  ← full log if needed"
        echo ""
        echo "Share summary-*.txt first — it's small enough to paste."
    fi
fi

exit $RC
