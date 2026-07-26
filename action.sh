#!/system/bin/sh
# Ternak TT — Action button
#   - Always: freshen persona
#   - Debug variant: also snapshot latest log to /sdcard for quick sharing
MODDIR="${0%/*}"

# Freshen (rotate persona)
"$MODDIR/bin/ternak-tt" freshen
RC=$?

# Debug-variant convenience: copy latest session log to /sdcard/Download
if [ -f "$MODDIR/debug_variant" ] && [ -d "$MODDIR/debug" ]; then
    LATEST=$(ls -1t "$MODDIR/debug"/session-*.log 2>/dev/null | head -1)
    if [ -n "$LATEST" ]; then
        OUTDIR="/sdcard/Download/ternak-tt-logs"
        mkdir -p "$OUTDIR" 2>/dev/null
        cp "$LATEST" "$OUTDIR/" 2>/dev/null
        [ -f "$MODDIR/debug/crashes.log" ] && cp "$MODDIR/debug/crashes.log" "$OUTDIR/" 2>/dev/null
        echo ""
        echo "[Ternak TT debug] latest log copied to:"
        echo "  $OUTDIR/$(basename $LATEST)"
        echo "  size: $(du -h $LATEST | cut -f1) / lines: $(wc -l < $LATEST)"
        [ -f "$MODDIR/debug/crashes.log" ] && echo "  crashes: $(wc -l < $MODDIR/debug/crashes.log) events journaled"
    fi
fi

exit $RC
