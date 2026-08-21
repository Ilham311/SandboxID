#!/system/bin/sh
MODDIR="${0%/*}"

BIN="$MODDIR/bin/sandboxid"
[ -x "$BIN" ] || BIN="$MODDIR/bin/sandboxid-arm64"

if [ -x "$BIN" ]; then
    "$BIN" seed >> /cache/sandboxid-boot.log 2>&1
fi
