#!/system/bin/sh
MODDIR="${0%/*}"
until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 2; done
sleep 5

# Multi-user prerequisites — unconditional every boot for MIUI/HyperOS.
# These must be applied BEFORE any create_profile.sh attempt because
# MIUI resets them on every boot. Do NOT nest inside the work_user_id
# check; a fresh install has no work_user_id yet but still needs
# these restrictions cleared for the first Create Profile to succeed.
MAX_USERS=$(getprop fw.max_users)
if [ -z "$MAX_USERS" ] || [ "$MAX_USERS" -lt 4 ]; then
    setprop fw.max_users 10
fi
setprop fw.show_multiuserui 1
pm set-user-restriction no_add_managed_profile 0 2>/dev/null
pm set-user-restriction no_add_user 0 2>/dev/null

# Silent-delete detection (keep existing logic, no restriction re-apply
# here anymore — moved to top-level above)
if [ -f "$MODDIR/work_user_id" ]; then
    WORK_ID=$(cat "$MODDIR/work_user_id")
    if ! pm list users | grep -q "UserInfo{$WORK_ID:"; then
        echo "Work profile $WORK_ID silently deleted by system." \
            >> /cache/ternak-tt-boot.log
        rm -f "$MODDIR/work_user_id"
    fi
fi

[ -f "$MODDIR/identity.prop" ] && [ -x "$MODDIR/bin/ternak-tt" ] && \
    "$MODDIR/bin/ternak-tt" apply-boot >> /cache/ternak-tt-boot.log 2>&1

if [ -f "$MODDIR/debug_variant" ]; then
    mkdir -p "$MODDIR/debug"
    chmod 0755 "$MODDIR/debug"

    ls -1t "$MODDIR/debug"/session-*.log 2>/dev/null | tail -n +6 | while read f; do
        rm -f "$f" "$f.gz" 2>/dev/null
    done

    TS=$(date +%Y%m%d-%H%M%S)
    LOGFILE="$MODDIR/debug/session-$TS.log"
    CRASHFILE="$MODDIR/debug/crashes.log"

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

    (
        sleep 8
        logcat -b main -b crash -b system -c 2>/dev/null
        logcat -b main -b crash -b system -v threadtime \
            -s TernakTT:V TernakTTCompanion:V AndroidRuntime:E DEBUG:V libc:F \
            >> "$LOGFILE" 2>&1
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
