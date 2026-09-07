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

LOG=/cache/sandboxid-boot.log
if [ -x "$BIN" ]; then
    {
        echo "[post-fs-data] seed begin"
        if "$BIN" seed; then
            echo "[post-fs-data] seed ok"
        else
            rc=$?
            echo "[post-fs-data] seed failed rc=$rc; apply-props skipped"
            exit "$rc"
        fi
        echo "[post-fs-data] apply-props begin"
        if "$BIN" apply-props; then
            echo "[post-fs-data] apply-props ok"
        else
            rc=$?
            echo "[post-fs-data] apply-props failed rc=$rc"
            exit "$rc"
        fi
    } >> "$LOG" 2>&1
fi
