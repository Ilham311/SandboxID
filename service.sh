#!/system/bin/sh
# Ternak TT — boot-time re-apply native prop + optional debug log capture
MODDIR="${0%/*}"
until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 2; done
sleep 5
[ -f "$MODDIR/identity.prop" ] && [ -x "$MODDIR/bin/ternak-tt" ] && \
    "$MODDIR/bin/ternak-tt" apply-boot >> /cache/ternak-tt-boot.log 2>&1

# --------------------------------------------------------------
# v1.0.10: auto-capture debug logs when debug variant is flashed.
# No user setup required — just install debug zip and reboot.
# Files land in $MODDIR/debug/session-<timestamp>.log
# Rotates: keeps newest 5 sessions.
# --------------------------------------------------------------
if [ -f "$MODDIR/debug_variant" ]; then
    mkdir -p "$MODDIR/debug"
    chmod 0755 "$MODDIR/debug"

    # Prune old sessions (keep newest 5)
    ls -1t "$MODDIR/debug"/session-*.log 2>/dev/null | tail -n +6 | while read f; do
        rm -f "$f" "$f.gz" 2>/dev/null
    done

    TS=$(date +%Y%m%d-%H%M%S)
    LOGFILE="$MODDIR/debug/session-$TS.log"
    CRASHFILE="$MODDIR/debug/crashes.log"

    # Session header — easy to spot when scrolling
    {
        echo "==================================================="
        echo "Ternak TT debug session"
        echo "Boot time : $(date '+%Y-%m-%d %H:%M:%S %Z')"
        echo "Uptime    : $(cat /proc/uptime 2>/dev/null | awk '{print $1"s"}')"
        echo "Module    : $(grep '^version=' $MODDIR/module.prop | cut -d= -f2)"
        echo "Kernel    : $(uname -r)"
        echo "Android   : $(getprop ro.build.version.release) (SDK $(getprop ro.build.version.sdk))"
        echo "ABI       : $(getprop ro.product.cpu.abi)"
        echo "==================================================="
        echo ""
    } > "$LOGFILE"
    chmod 0644 "$LOGFILE"

    # Background logcat writer — filters our tags, verbose level.
    # Extra tags: AndroidRuntime:E catches Java crashes, DEBUG:V catches
    # native tombstone headers, libc:F catches fortify aborts.
    (
        sleep 8
        logcat -b main -b crash -b system -c 2>/dev/null
        logcat -b main -b crash -b system -v threadtime \
            -s TernakTT:V TernakTTCompanion:V AndroidRuntime:E DEBUG:V libc:F \
            >> "$LOGFILE" 2>&1
    ) &
    echo "$!" > "$MODDIR/debug/logcat.pid"

    # Persistent crash/death journal — append-only across all sessions.
    (
        sleep 10
        touch "$CRASHFILE"
        chmod 0644 "$CRASHFILE"
        tail -F "$LOGFILE" 2>/dev/null | grep --line-buffered -E 'CRASH|DEATH|LEAK' >> "$CRASHFILE" 2>&1
    ) &
    echo "$!" > "$MODDIR/debug/journal.pid"
fi
