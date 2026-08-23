#!/system/bin/sh
MODDIR="${0%/*}"
until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 2; done
sleep 5
# Ship idle: the device-wide apply-boot layer runs only when the user has
# activated the module (at least one active entry in target.txt). With an empty
# target.txt the module stays inert -- no ~70 resetprop/settings writes happen.
if grep -qE '^[[:space:]]*[^[:space:]#]' "$MODDIR/target.txt" 2>/dev/null; then
    [ -f "$MODDIR/identity.prop" ] && [ -x "$MODDIR/bin/sandboxid" ] && \
        "$MODDIR/bin/sandboxid" apply-boot >> /cache/sandboxid-boot.log 2>&1
fi

if [ -f "$MODDIR/debug_variant" ]; then
    mkdir -p "$MODDIR/debug"
    chmod 0755 "$MODDIR/debug"

    # keep only the 5 newest session logs
    ls -1t "$MODDIR/debug"/session-*.log 2>/dev/null | tail -n +6 | while read f; do
        rm -f "$f" 2>/dev/null
    done
    # one-time sweep: buang sisa arsip .log.gz dari versi lama (fitur gz dihapus)
    rm -f "$MODDIR/debug"/session-*.log.gz "$MODDIR/debug/report"/*.log.gz 2>/dev/null

    TS=$(date +%Y%m%d-%H%M%S)
    LOGFILE="$MODDIR/debug/session-$TS.log"
    CRASHFILE="$MODDIR/debug/crashes.log"

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

    (
        sleep 10
        touch "$CRASHFILE"
        chmod 0644 "$CRASHFILE"
        # NOTE: use awk, not `grep --line-buffered`. On Android the grep in PATH
        # is BusyBox/toybox, and NEITHER supports GNU's --line-buffered flag --
        # it aborts with "unrecognized option" and crashes.log is never written.
        # awk is present in both BusyBox and toybox; fflush() gives the same
        # per-line flush so a crash line lands in crashes.log immediately.
        tail -F "$LOGFILE" 2>/dev/null | awk '/CRASH|DEATH|LEAK/ { print; fflush() }' >> "$CRASHFILE" 2>&1
    ) &
    echo "$!" > "$MODDIR/debug/journal.pid"
fi
