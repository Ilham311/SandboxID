#!/system/bin/sh
MODDIR="${0%/*}"
until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 2; done
sleep 5

# No global property application at boot: spoofing is per-target and in-process
# now. identity.prop is ensured by post-fs-data.sh (seed) and, failing that, by
# the companion on-demand at first target launch.

if [ -f "$MODDIR/debug_variant" ]; then
    mkdir -p "$MODDIR/debug"
    chmod 0755 "$MODDIR/debug"

    ls -1t "$MODDIR/debug"/session-*.log 2>/dev/null | tail -n +6 | while read f; do
        rm -f "$f" 2>/dev/null
    done
    rm -f "$MODDIR/debug"/session-*.log.gz "$MODDIR/debug/report"/*.log.gz 2>/dev/null

    TS=$(date +%Y%m%d-%H%M%S)
    LOGFILE="$MODDIR/debug/session-$TS.log"
    CRASHFILE="$MODDIR/debug/crashes.log"

    # The crash ledger accumulates across sessions; bound it so it cannot grow
    # without limit inside the module directory. The full text of every crash
    # is still in the rotated session logs.
    if [ -f "$CRASHFILE" ] && [ "$(wc -c < "$CRASHFILE" 2>/dev/null)" -gt 262144 ]; then
        tail -n 2000 "$CRASHFILE" > "$CRASHFILE.tmp" 2>/dev/null && mv -f "$CRASHFILE.tmp" "$CRASHFILE"
    fi

    {
        echo "==================================================="
        echo "SandboxID debug session"
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

    (
        sleep 8
        logcat -b main -b crash -b system -c 2>/dev/null
        logcat -b main -b crash -b system -v threadtime \
            -s SandboxID:V SandboxIDCompanion:V AndroidRuntime:E DEBUG:V libc:F \
            >> "$LOGFILE" 2>&1
    ) &
    echo "$!" > "$MODDIR/debug/logcat.pid"

    # Crash extractor. NOTE: the patterns below are the markers that actually
    # appear in the session log - the old '/CRASH|DEATH|LEAK/' filter matched
    # nothing the module or the platform ever emits, so crashes.log stayed
    # empty even while the session log contained a full tombstone. index() is
    # used instead of an ERE so the literal banner survives awk dialect
    # differences, and -n 0 starts at EOF so nothing logcat has not written
    # yet is skipped.
    (
        touch "$CRASHFILE"
        chmod 0644 "$CRASHFILE"
        {
            echo ""
            echo "=== session $TS (module $(grep '^version=' "$MODDIR/module.prop" | cut -d= -f2)) ==="
        } >> "$CRASHFILE"
        tail -F -n 0 "$LOGFILE" 2>/dev/null | awk '
            index($0, "SandboxID CRASH") ||
            index($0, "Fatal signal") ||
            index($0, "*** *** *** *** *** *** ***") ||
            index($0, "Cmdline:") ||
            index($0, ">>> ") ||
            index($0, "Build fingerprint:") ||
            index($0, " pc ") ||
            index($0, "FATAL EXCEPTION") { print; fflush() }
        ' >> "$CRASHFILE" 2>&1
    ) &
    echo "$!" > "$MODDIR/debug/journal.pid"
fi
