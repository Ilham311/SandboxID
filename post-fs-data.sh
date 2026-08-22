#!/system/bin/sh
MODDIR="${0%/*}"

BIN="$MODDIR/bin/sandboxid"
[ -x "$BIN" ] || BIN="$MODDIR/bin/sandboxid-arm64"

# Ship idle: only seed a persona when the user has activated the module by adding
# at least one package to target.txt. An empty (or comment-only) target.txt keeps
# the module fully inert -- no identity.prop is generated, so service.sh's
# apply-boot stays a no-op too. This makes behavior match the documented contract
# "empty target.txt = module idle".
TARGET="$MODDIR/target.txt"
grep -qE '^[[:space:]]*[^[:space:]#]' "$TARGET" 2>/dev/null || exit 0

if [ -x "$BIN" ]; then
    "$BIN" seed >> /cache/sandboxid-boot.log 2>&1
fi
