#!/system/bin/sh
MODDIR="${0%/*}"

if [ -f "$MODDIR/work_user_id" ]; then
    WORK_ID=$(cat "$MODDIR/work_user_id")
    echo "Removing work profile $WORK_ID..." >> /cache/ternak-tt-uninstall.log
    pm remove-user "$WORK_ID" >> /cache/ternak-tt-uninstall.log 2>&1
fi
