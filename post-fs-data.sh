#!/system/bin/sh
MODDIR="${0%/*}"

BIN="$MODDIR/bin/sandboxid"
if [ ! -x "$BIN" ]; then
    case "$(getprop ro.product.cpu.abi)" in
        arm64-v8a)   BIN="$MODDIR/bin/sandboxid-arm64" ;;
        armeabi-v7a) BIN="$MODDIR/bin/sandboxid-arm" ;;
        x86_64)      BIN="$MODDIR/bin/sandboxid-x86_64" ;;
        x86)         BIN="$MODDIR/bin/sandboxid-x86" ;;
    esac
fi

TARGET="$MODDIR/target.txt"
grep -qE '^[[:space:]]*[^[:space:]#]' "$TARGET" 2>/dev/null || exit 0

if [ -x "$BIN" ]; then
    "$BIN" seed >> /cache/sandboxid-boot.log 2>&1
fi
