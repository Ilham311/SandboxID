#!/system/bin/sh
# ================================================================
# Ternak TT — session log summarizer
#
# Reads a raw session-<ts>.log (potentially 10+ MB) and emits a
# compact digest (~5-15 KB) suitable for copy-pasting into chat.
#
# Usage:
#   summarize.sh <session-log> [output-file]
#
# Called automatically by action.sh when Action button is tapped.
# Safe under toybox / busybox on Android (no bashisms).
# ================================================================

IN="$1"
OUT="${2:-/proc/self/fd/1}"

if [ -z "$IN" ] || [ ! -f "$IN" ]; then
    echo "usage: $0 <session-log> [output-file]" >&2
    exit 1
fi

TOTAL_LINES=$(wc -l < "$IN")
TOTAL_SIZE=$(du -h "$IN" | cut -f1)

{
    echo "==============================================================="
    echo "Ternak TT — session log summary"
    echo "Source  : $(basename $IN)"
    echo "Size    : $TOTAL_SIZE ($TOTAL_LINES lines)"
    echo "Digest  : $(date '+%Y-%m-%d %H:%M:%S %Z')"
    echo "==============================================================="
    echo ""

    # -----------------------------------------------------------------
    # Session header (top block written by service.sh)
    # -----------------------------------------------------------------
    echo "--- Session header ---"
    head -15 "$IN" | grep -vE '^\s*$'
    echo ""

    # -----------------------------------------------------------------
    # High-level event counters
    # -----------------------------------------------------------------
    echo "--- Event counts ---"
    printf "  SPOOF   : %s\n"  "$(grep -c 'SPOOF' "$IN")"
    printf "  LEAK    : %s\n"  "$(grep -c 'LEAK'  "$IN")"
    printf "  MISS    : %s\n"  "$(grep -c 'MISS'  "$IN")"
    printf "  CRASH   : %s\n"  "$(grep -c 'CRASH \[' "$IN")"
    printf "  DEATH   : %s\n"  "$(grep -c 'DEATH target' "$IN")"
    printf "  MOUNT   : %s\n"  "$(grep -Ec 'child mount for pid=|bind-mount via companion|bind OK:' "$IN")"
    printf "  ANR     : %s\n"  "$(grep -c 'ANR in' "$IN")"
    printf "  FATAL   : %s\n"  "$(grep -c 'FATAL EXCEPTION' "$IN")"
    echo ""

    # -----------------------------------------------------------------
    # Targets
    # -----------------------------------------------------------------
    echo "--- Target packages seen ---"
    grep -oE 'pkg=[a-zA-Z0-9._]+' "$IN" \
        | sort | uniq -c | sort -rn \
        | awk '{printf "  %-6s %s\n", $1"x", $2}' \
        | sed 's/pkg=//'
    echo ""

    # -----------------------------------------------------------------
    # Unique LEAK surfaces — the MOST useful section for debugging.
    # These are the queries TikTok/Grab made that we didn't hook.
    # -----------------------------------------------------------------
    echo "--- Unique LEAK surfaces (top 40, sorted by frequency) ---"
    grep 'LEAK' "$IN" \
        | sed -E "s/.*(L[0-9] [A-Za-z_.]+[^']*'[^']+').*/\\1/" \
        | grep -E '^L[0-9]' \
        | sort | uniq -c | sort -rn | head -40 \
        | awk '{n=$1; $1=""; sub(/^ /,""); printf "  %5dx  %s\n", n, $0}'
    echo ""

    # -----------------------------------------------------------------
    # SPOOF distribution — verify our hooks are firing
    # -----------------------------------------------------------------
    echo "--- SPOOF hits per hook layer ---"
    for L in L1 L2 L3 L4 L5 L6 L7; do
        C=$(grep -c "$L SPOOF\|$L install_" "$IN")
        [ "$C" -gt 0 ] && printf "  %s : %s\n" "$L" "$C"
    done
    echo ""

    # -----------------------------------------------------------------
    # ALL crash events (full lines — usually 0-5)
    # -----------------------------------------------------------------
    echo "--- CRASH events ---"
    CN=$(grep -c 'CRASH \[' "$IN")
    if [ "$CN" -gt 0 ]; then
        grep 'CRASH \[' "$IN" | awk '{sub(/^.*E TernakTT: /, ""); print "  " $0}'
    else
        echo "  (none)"
    fi
    echo ""

    # -----------------------------------------------------------------
    # ALL death events
    # -----------------------------------------------------------------
    echo "--- DEATH events ---"
    DN=$(grep -c 'DEATH target' "$IN")
    if [ "$DN" -gt 0 ]; then
        grep 'DEATH target' "$IN" | awk '{sub(/^.*I TernakTTCompanion: /, ""); print "  " $0}'
    else
        echo "  (none)"
    fi
    echo ""

    # -----------------------------------------------------------------
    # Java exceptions + native tombstone hints
    # -----------------------------------------------------------------
    echo "--- Java FATAL EXCEPTION (first 15 stack lines) ---"
    FN=$(grep -c 'FATAL EXCEPTION' "$IN")
    if [ "$FN" -gt 0 ]; then
        awk '/FATAL EXCEPTION/{flag=15} flag>0{print "  " $0; flag--}' "$IN" | head -60
    else
        echo "  (none)"
    fi
    echo ""

    echo "--- Native tombstone hints (DEBUG tag) ---"
    TN=$(grep -c 'F DEBUG' "$IN")
    if [ "$TN" -gt 0 ]; then
        grep -E 'F DEBUG.*(signal|Abort message|backtrace|pid:|Cmdline)' "$IN" \
            | head -20 | awk '{sub(/^.*F DEBUG   : /, ""); print "  " $0}'
    else
        echo "  (none)"
    fi
    echo ""

    # -----------------------------------------------------------------
    # Companion mount timeline
    # -----------------------------------------------------------------
    echo "--- Companion mount events ---"
    grep -E 'child mount for pid=|bind-mount via companion|bind OK:|bind fail|setns->target|child: setns OK|child: open /proc/' "$IN" \
        | head -25 | awk '{sub(/^.*(TernakTTCompanion|TernakTT): /, ""); print "  " $0}'
    echo ""

    # -----------------------------------------------------------------
    # Last 30 lines — usually the most recent activity / crash
    # -----------------------------------------------------------------
    echo "--- Last 30 lines of session ---"
    tail -30 "$IN" | awk '{print "  " $0}'
    echo ""

    echo "==============================================================="
    echo "End of summary.  If you need the full log, gzip it and share:"
    echo "  gzip -c $IN > /sdcard/Download/session.log.gz"
    echo "==============================================================="

} > "$OUT"

# Emit final size to stderr for the caller
if [ "$OUT" != "/proc/self/fd/1" ]; then
    echo "summary: $OUT ($(du -h $OUT | cut -f1), $(wc -l < $OUT) lines)" >&2
fi
