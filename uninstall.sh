#!/system/bin/sh
MODDIR="/data/adb/modules/ternak_tt"
LOG="/cache/ternak-tt-uninstall.log"

echo "[$(date '+%Y-%m-%d %H:%M:%S')] START: uninstall cleanup" \
    >> "$LOG"

if [ -f "$MODDIR/work_user_id" ]; then
    WORK_ID=$(cat "$MODDIR/work_user_id")
    echo "Removing work profile user $WORK_ID..." >> "$LOG"
    pm remove-user "$WORK_ID" >> "$LOG" 2>&1
    rm -f "$MODDIR/work_user_id"
else
    echo "No work_user_id found — skipping profile removal." >> "$LOG"
fi

echo "[$(date '+%Y-%m-%d %H:%M:%S')] OK: uninstall cleanup done" \
    >> "$LOG"
exit 0
