#!/system/bin/sh

MODDIR="${0%/*}"
BIN="$MODDIR/bin/sandboxid"
ROTATE="$MODDIR/rotate_ids.sh"
LOGFILE="/cache/sandboxid-boot.log"
[ -w "$(dirname "$LOGFILE")" ] || LOGFILE="/data/local/tmp/sandboxid-boot.log"
touch "$LOGFILE" 2>/dev/null

ACTION_LOG="$MODDIR/debug/action.log"
mkdir -p "$MODDIR/debug" 2>/dev/null
{
  echo ""
  echo "=== $(date '+%F %T') action.sh (moddir=$MODDIR) ==="
} >> "$ACTION_LOG" 2>/dev/null

RC_FRESHEN=0
RC_ROTATE=0

# Step 0/2: best-effort refresh of the persona pool (personas.tsv) from Google's
# live Pixel build data. autopif.sh is a NO-OP when the device has no curl/wget
# (the common case) -- the bundled stable pool stays in force -- and it always
# exits 0, so it can never block freshen.
AUTOPIF="$MODDIR/autopif.sh"
if [ -f "$AUTOPIF" ]; then
    echo "[SandboxID] refresh persona pool (autopif, best-effort)..."
    MODDIR="$MODDIR" sh "$AUTOPIF" 2>&1 | tee -a "$LOGFILE" "$ACTION_LOG"
fi

if [ -x "$BIN" ]; then
    echo "[SandboxID] freshen (step 1/2)..."
    "$BIN" unlock >/dev/null 2>&1 || true
    
    
    FRESHEN_OUT="$MODDIR/debug/.freshen.$$"
    "$BIN" freshen < /dev/null > "$FRESHEN_OUT" 2>&1; RC_FRESHEN=$?
    tee -a "$LOGFILE" "$ACTION_LOG" < "$FRESHEN_OUT"
    rm -f "$FRESHEN_OUT" 2>/dev/null
    "$BIN" lock >/dev/null 2>&1 || true
    echo "[SandboxID] auto-locked after freshen" | tee -a "$LOGFILE" "$ACTION_LOG"
else
    echo "[SandboxID] ERROR: $BIN not executable" | tee -a "$LOGFILE"
    RC_FRESHEN=127
fi

if [ -r "$ROTATE" ]; then
    echo "[SandboxID] rotate_ids all (step 2/2)..."
    ROTATE_OUT="$MODDIR/debug/.rotate.$$"
    MODDIR="$MODDIR" LOGFILE="$LOGFILE" sh "$ROTATE" all < /dev/null > "$ROTATE_OUT" 2>&1; RC_ROTATE=$?
    tee -a "$LOGFILE" "$ACTION_LOG" < "$ROTATE_OUT"
    rm -f "$ROTATE_OUT" 2>/dev/null
else
    echo "[SandboxID] WARN: rotate_ids.sh missing - skipping shell-layer rotation" | tee -a "$LOGFILE"
fi

echo ""
echo "[SandboxID] freshen rc=$RC_FRESHEN, rotate rc=$RC_ROTATE"
if [ "$RC_ROTATE" != "0" ] && [ "$RC_ROTATE" != "1" ]; then
    echo "[SandboxID] see $LOGFILE for details"
fi

if [ -f "$MODDIR/debug_variant" ] && [ -d "$MODDIR/debug" ]; then
    LATEST=$(ls -1t "$MODDIR/debug"/session-*.log 2>/dev/null | head -1)
    if [ -n "$LATEST" ]; then
        
        
        
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
        echo "[SandboxID debug artifacts] (root-only)"
        echo "  dir: $OUTDIR/"
        [ -f "$SUMMARY" ] && echo "  ok summary  $(basename $SUMMARY)  ($(du -h $SUMMARY | cut -f1))"
        [ -f "$OUTDIR/crashes-$TS.log" ] && echo "  ok crashes  crashes-$TS.log  ($(du -h $OUTDIR/crashes-$TS.log | cut -f1))"
        [ -f "$RAW_GZ" ] && echo "  ok raw.gz   $(basename $RAW_GZ)  ($(du -h $RAW_GZ | cut -f1))"
        echo ""
        echo "View via WebUI Log tab, or pull with a root file manager."
    fi
fi

exit $RC_FRESHEN
