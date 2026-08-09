#!/system/bin/sh
MODDIR="/data/adb/modules/ternak_tt"
LOG="$MODDIR/wp_action.log"
TARGET_PKG="$1"

echo "[$(date '+%Y-%m-%d %H:%M:%S')] START: add_target $TARGET_PKG" | tee -a "$LOG"

if [ -z "$TARGET_PKG" ]; then
    echo "ERROR: Missing target package name." | tee -a "$LOG"
    exit 1
fi

if [ ! -f "$MODDIR/work_user_id" ]; then
    echo "ERROR: work_user_id not found. Create a work profile first." | tee -a "$LOG"
    exit 1
fi

WORK_ID=$(cat "$MODDIR/work_user_id")

if ! pm list packages | grep -q "package:$TARGET_PKG"; then
     echo "ERROR: Package $TARGET_PKG not installed on device." | tee -a "$LOG"
     exit 1
fi

echo "Installing $TARGET_PKG to user $WORK_ID..." | tee -a "$LOG"
pm install-existing --user "$WORK_ID" "$TARGET_PKG" >> "$LOG" 2>&1
if [ $? -ne 0 ]; then
    echo "ERROR: Failed to install target to work profile." | tee -a "$LOG"
    exit 1
fi

echo "[$(date '+%Y-%m-%d %H:%M:%S')] OK: Target $TARGET_PKG added to work profile $WORK_ID" | tee -a "$LOG"
exit 0
