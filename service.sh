#!/system/bin/sh
# Ternak TT — boot-time re-apply native prop
MODDIR="${0%/*}"
until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 2; done
sleep 5
[ -f "$MODDIR/identity.prop" ] && [ -x "$MODDIR/bin/ternak-tt" ] && \
    "$MODDIR/bin/ternak-tt" apply-boot >> /cache/ternak-tt-boot.log 2>&1
