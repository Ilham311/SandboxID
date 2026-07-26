#!/system/bin/sh
# Ternak TT — Action button
#   - Always: freshen persona
#   - Debug variant: auto-summarize latest log + copy to /sdcard
MODDIR="${0%/*}"

# 1. Freshen (rotate persona)
"$MODDIR/bin/ternak-tt" freshen
RC=$?

# 2. Debug-variant convenience: produce a compact summary + gzip raw log
if [ -f "$MODDIR/debug_variant" ] && [ -d "$MODDIR/debug" ]; then
    LATEST=$(ls -1t "$MODDIR/debug"/session-*.log 2>/dev/null | head -1)
    if [ -n "$LATEST" ]; then
        OUTDIR="/sdcard/Download/ternak-tt-logs"
        mkdir -p "$OUTDIR" 2>/dev/null
        TS=$(date +%Y%m%d-%H%M%S)

        # ---- a) Small, copy-pasteable summary (~5-15 KB) ----
        SUMMARY="$OUTDIR/summary-$TS.txt"
        if [ -x "$MODDIR/summarize.sh" ] || [ -f "$MODDIR/summarize.sh" ]; then
            sh "$MODDIR/summarize.sh" "$LATEST" "$SUMMARY" 2>/dev/null
        fi

        # ---- b) Persistent crash/death/leak journal ----
        if [ -f "$MODDIR/debug/crashes.log" ]; then
            cp "$MODDIR/debug/crashes.log" "$OUTDIR/crashes-$TS.log" 2>/dev/null
        fi

        # ---- c) Raw session log gzipped (for deep dives) ----
        RAW_GZ="$OUTDIR/session-$TS.log.gz"
        gzip -c "$LATEST" > "$RAW_GZ" 2>/dev/null

        # ---- d) Prune old artifacts in /sdcard (keep newest 10 of each) ----
        for pattern in "summary-*.txt" "crashes-*.log" "session-*.log.gz"; do
            ls -1t $OUTDIR/$pattern 2>/dev/null | tail -n +11 | xargs rm -f 2>/dev/null
        done

        # ---- e) Print user-facing status ----
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
