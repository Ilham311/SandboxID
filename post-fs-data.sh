#!/system/bin/sh
MODDIR="${0%/*}"

BIN="$MODDIR/bin/ternak-tt"
if [ ! -x "$BIN" ]; then
    ABI=$(getprop ro.product.cpu.abi)
    case "$ABI" in
        arm64-v8a)   BIN="$MODDIR/bin/ternak-tt-arm64" ;;
        armeabi-v7a) BIN="$MODDIR/bin/ternak-tt-arm" ;;
        x86_64)      BIN="$MODDIR/bin/ternak-tt-x86_64" ;;
        x86)         BIN="$MODDIR/bin/ternak-tt-x86" ;;
    esac
fi

if [ -x "$BIN" ]; then
    timeout 3 "$BIN" seed >> /cache/ternak-tt-boot.log 2>&1 || exit 0
fi
