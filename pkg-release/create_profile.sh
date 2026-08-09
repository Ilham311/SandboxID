#!/system/bin/sh
MODDIR="/data/adb/modules/ternak_tt"
LOG="$MODDIR/wp_action.log"
mkdir -p "$MODDIR"
touch "$LOG"
chmod 0644 "$LOG"

echo "[$(date '+%Y-%m-%d %H:%M:%S')] START: create_profile" | tee -a "$LOG"

# 2. Bump fw.max_users + show_multiuserui
setprop fw.max_users 10
setprop fw.show_multiuserui 1

# 3. Clear no_add_managed_profile + no_add_user restrictions
pm set-user-restriction no_add_managed_profile 0
pm set-user-restriction no_add_user 0

# 4. Guard: check no Device Owner exists
if dumpsys device_policy | grep -q "Device Owner"; then
    echo "ERROR: Device Owner detected. Managed profile cannot be created while a Device Owner is active. Please remove the Device Owner first." | tee -a "$LOG"
    exit 2
fi

# 5. Guard: check work_user_id doesn't already exist
if [ -f "$MODDIR/work_user_id" ]; then
    echo "ERROR: work_user_id already exists. Profile might be already provisioned." | tee -a "$LOG"
    exit 1
fi

# 6. pm create-user
echo "Creating user..." | tee -a "$LOG"
CREATE_OUT=$(pm create-user --profileOf 0 --managed "TernakTT" 2>&1)
echo "Output: $CREATE_OUT" | tee -a "$LOG"

NEW_ID=$(echo "$CREATE_OUT" | grep -o 'id [0-9]*' | awk '{print $2}')
if [ -z "$NEW_ID" ]; then
    echo "ERROR: Failed to parse user id from pm create-user output." | tee -a "$LOG"
    exit 1
fi
echo "New user ID: $NEW_ID" | tee -a "$LOG"

# 7. Persist user id
echo "$NEW_ID" > "$MODDIR/work_user_id"

# 8. pm install-existing
echo "Installing DPC to user $NEW_ID..." | tee -a "$LOG"
pm install-existing --user "$NEW_ID" com.ternak.tt.dpc >> "$LOG" 2>&1

# 9. dpm set-profile-owner
echo "Setting profile owner..." | tee -a "$LOG"
dpm set-profile-owner --user "$NEW_ID" com.ternak.tt.dpc/.TtDeviceAdminReceiver >> "$LOG" 2>&1

# 10. am start-user
echo "Starting user $NEW_ID..." | tee -a "$LOG"
am start-user "$NEW_ID" >> "$LOG" 2>&1

# 11. Verify
sleep 2 # give it a moment to start
if ! pm list users | grep -q "${NEW_ID}.*running"; then
    echo "ERROR: User $NEW_ID failed to start or is not running." | tee -a "$LOG"
    exit 3
fi

# 12. Log OK
echo "[$(date '+%Y-%m-%d %H:%M:%S')] OK: Work profile created with user id $NEW_ID" | tee -a "$LOG"
exit 0
