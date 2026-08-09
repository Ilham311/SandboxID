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

# 1. Append to target.txt and wp_added_targets.txt
touch "$MODDIR/target.txt" "$MODDIR/wp_added_targets.txt"
if ! grep -qxF "$TARGET_PKG" "$MODDIR/target.txt"; then
    echo "$TARGET_PKG" >> "$MODDIR/target.txt"
    echo "$TARGET_PKG" >> "$MODDIR/wp_added_targets.txt"
    echo "Appended $TARGET_PKG to target.txt" | tee -a "$LOG"
fi

# 2. Regenerate bloom filter
if [ -x "$MODDIR/bin/ternak-tt" ]; then
    "$MODDIR/bin/ternak-tt" seed >> "$LOG" 2>&1
    echo "Regenerated bloom filter." | tee -a "$LOG"
else
    echo "WARNING: ternak-tt binary not found, could not regenerate bloom filter." | tee -a "$LOG"
fi

# 3. Print user-facing hint
echo "HINT: Restart target app or reboot to activate hook." | tee -a "$LOG"

echo "[$(date '+%Y-%m-%d %H:%M:%S')] OK: Target $TARGET_PKG added to work profile $WORK_ID" | tee -a "$LOG"
exit 0
