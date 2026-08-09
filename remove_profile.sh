#!/system/bin/sh
MODDIR="/data/adb/modules/ternak_tt"
LOG="$MODDIR/wp_action.log"

echo "[$(date '+%Y-%m-%d %H:%M:%S')] START: remove_profile" | tee -a "$LOG"

if [ ! -f "$MODDIR/work_user_id" ]; then
    echo "ERROR: work_user_id not found. No profile to remove." | tee -a "$LOG"
    exit 1
fi

WORK_ID=$(cat "$MODDIR/work_user_id")

echo "Removing user $WORK_ID..." | tee -a "$LOG"
pm remove-user "$WORK_ID" >> "$LOG" 2>&1

rm -f "$MODDIR/work_user_id"

# Clean up packages in target.txt that were added via add_target.sh
if [ -f "$MODDIR/wp_added_targets.txt" ]; then
    while IFS= read -r pkg; do
        if [ -n "$pkg" ] && [ -f "$MODDIR/target.txt" ]; then
            sed -i "/^${pkg}$/d" "$MODDIR/target.txt"
        fi
    done < "$MODDIR/wp_added_targets.txt"
    rm -f "$MODDIR/wp_added_targets.txt"

    if [ -x "$MODDIR/bin/ternak-tt" ]; then
        "$MODDIR/bin/ternak-tt" seed >> "$LOG" 2>&1
        echo "Regenerated bloom filter after target cleanup." | tee -a "$LOG"
    fi
fi

echo "[$(date '+%Y-%m-%d %H:%M:%S')] OK: Profile $WORK_ID removed" | tee -a "$LOG"
exit 0
