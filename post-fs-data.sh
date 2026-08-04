#!/system/bin/sh
MODDIR="${0%/*}"

BIN="$MODDIR/bin/ternak-tt"
[ -x "$BIN" ] || BIN="$MODDIR/bin/ternak-tt-arm64"

if [ -x "$BIN" ]; then
    "$BIN" seed >> /cache/ternak-tt-boot.log 2>&1
fi
