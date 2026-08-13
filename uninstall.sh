#!/system/bin/sh

MODDIR="/data/adb/modules/ternak_tt"
LOG_OUT="/sdcard/Download/ternak_tt_uninstall_$(date +%Y%m%d_%H%M%S).log"

_log() {
    ui_print "$1" 2>/dev/null || true
    echo "$1"
    [ -w /sdcard/Download ] && echo "$1" >> "$LOG_OUT"
}

_log "- Ternak TT Uninstall"

rm -f /cache/ternak-tt-boot.log
rm -f /data/local/tmp/ternak-tt-boot.log
_log "- Removed boot logs"

if [ -d "$MODDIR/backups" ]; then
    _log "- Backups found at $MODDIR/backups:"
    for f in "$MODDIR/backups"/*; do
        [ -e "$f" ] && _log "  $f"
    done
    _log "- (These backups will be deleted when Magisk removes the module directory)"
fi

_log "- Uninstall cleanup complete."
