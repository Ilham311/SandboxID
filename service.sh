#!/system/bin/sh
MODDIR="${0%/*}"
until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 2; done
sleep 5
BIN="$MODDIR/bin/sandboxid"
if [ ! -x "$BIN" ]; then
    case "$(getprop ro.product.cpu.abi)" in
        arm64-v8a)   BIN="$MODDIR/bin/sandboxid-arm64" ;;
        armeabi-v7a) BIN="$MODDIR/bin/sandboxid-arm" ;;
        x86_64)      BIN="$MODDIR/bin/sandboxid-x86_64" ;;
        x86)         BIN="$MODDIR/bin/sandboxid-x86" ;;
    esac
fi

if grep -qE '^[[:space:]]*[^[:space:]#]' "$MODDIR/target.txt" 2>/dev/null; then
    [ -f "$MODDIR/identity.prop" ] && [ -x "$BIN" ] && \
        "$BIN" apply-boot >> /cache/sandboxid-boot.log 2>&1
fi

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
        tail -F "$LOGFILE" 2>/dev/null | awk '/CRASH|DEATH|LEAK/ { print; fflush() }' >> "$CRASHFILE" 2>&1
    ) &
    echo "$!" > "$MODDIR/debug/journal.pid"
fi
