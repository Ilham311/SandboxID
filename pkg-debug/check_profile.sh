#!/system/bin/sh
MODDIR="/data/adb/modules/ternak_tt"
LOG="$MODDIR/wp_action.log"

echo "[$(date '+%Y-%m-%d %H:%M:%S')] START: check_profile" | tee -a "$LOG"

if [ ! -f "$MODDIR/work_user_id" ]; then
    echo "STATUS: No work profile active (work_user_id missing)." | tee -a "$LOG"
    exit 0
fi

WORK_ID=$(cat "$MODDIR/work_user_id")

if ! pm list users | grep -q "UserInfo{$WORK_ID:"; then
    echo "STATUS: Inconsistent state. User $WORK_ID not found in system, but work_user_id exists." | tee -a "$LOG"
    exit 1
fi

if pm list users | grep -q "${WORK_ID}.*running"; then
    echo "STATUS: Work profile $WORK_ID is active and running." | tee -a "$LOG"
else
    echo "STATUS: Work profile $WORK_ID exists but is NOT running." | tee -a "$LOG"
fi

exit 0
