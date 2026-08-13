#!/system/bin/sh
MODDIR="${0%/*}"
max_wait=150
iters=0
until [ "$(getprop sys.boot_completed)" = "1" ]; do
    sleep 2
    iters=$((iters + 1))
    if [ "$iters" -ge "$max_wait" ]; then
        exit 0
    fi
done
sleep 5
[ -f "$MODDIR/identity.prop" ] && [ -x "$MODDIR/bin/ternak-tt" ] && \
    "$MODDIR/bin/ternak-tt" apply-boot >> /cache/ternak-tt-boot.log 2>&1

if [ -f "$MODDIR/debug_variant" ]; then
    mkdir -p "$MODDIR/debug"
    chmod 0755 "$MODDIR/debug"

    # shellcheck disable=SC2012
    ls -1t "$MODDIR"/debug/session-*.log 2>/dev/null | tail -n +6 | while read -r f; do
        rm -f "$f" "$f.gz" 2>/dev/null
    done

    TS=$(date +%Y%m%d-%H%M%S)
    LOGFILE="$MODDIR/debug/session-$TS.log"
    CRASHFILE="$MODDIR/debug/crashes.log"

    {
        echo "==================================================="
        echo "Ternak TT debug session"
        echo "Boot time : $(date '+%Y-%m-%d %H:%M:%S %Z')"
        # shellcheck disable=SC2002
        echo "Uptime    : $(cat /proc/uptime 2>/dev/null | awk '{print $1"s"}')"
        echo "Module    : $(grep '^version=' "$MODDIR"/module.prop | cut -d= -f2)"
        echo "Kernel    : $(uname -r)"
        echo "Android   : $(getprop ro.build.version.release) (SDK $(getprop ro.build.version.sdk))"
        echo "ABI       : $(getprop ro.product.cpu.abi)"
        echo "==================================================="
        echo ""
    } > "$LOGFILE"
    chmod 0644 "$LOGFILE"

    if [ -f "$MODDIR/debug/logcat.pid" ]; then
        kill "$(cat "$MODDIR/debug/logcat.pid")" 2>/dev/null
        rm -f "$MODDIR/debug/logcat.pid"
    fi
    if [ -f "$MODDIR/debug/journal.pid" ]; then
        kill "$(cat "$MODDIR/debug/journal.pid")" 2>/dev/null
        rm -f "$MODDIR/debug/journal.pid"
    fi

    (
        sleep 8
        logcat -b main -b crash -b system -v threadtime -T 1 \
            -s TernakTT:V TernakTTCompanion:V AndroidRuntime:E DEBUG:V libc:F \
            2>&1 | head -c 20M >> "$LOGFILE"
    ) &
    echo "$!" > "$MODDIR/debug/logcat.pid"

    (
        sleep 10
        touch "$CRASHFILE"
        chmod 0644 "$CRASHFILE"
        tail -F "$LOGFILE" 2>/dev/null | grep --line-buffered -E 'CRASH|DEATH|LEAK' >> "$CRASHFILE" 2>&1
    ) &
    echo "$!" > "$MODDIR/debug/journal.pid"
fi
