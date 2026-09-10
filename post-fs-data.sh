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

LOG="${LOGFILE:-/cache/sandboxid-boot.log}"
[ -w "$(dirname "$LOG")" ] || LOG="$MODDIR/sandboxid-boot.log"
if [ -x "$BIN" ]; then
    if [ -r "$MODDIR/.operational-flags" ] && [ -f "$MODDIR/identity.prop" ]; then
        while IFS='=' read -r key value; do
            case "$key:$value" in
                SBX_NATIVE_READ:[01]|SBX_HIDE:[01]|SBX_CPU_REVISION:[01]|\
                SBX_PROC_VERSION:[01]|SBX_MEMINFO:[01])
                    "$BIN" set-flag "$key" "$value" >> "$LOG" 2>&1 || echo "[post-fs-data] set-flag $key failed rc=$?" >> "$LOG" 2>&1
                    ;;
            esac
        done < "$MODDIR/.operational-flags"
        rm -f "$MODDIR/.operational-flags"
    fi

    grep -qE '^[[:space:]]*[^[:space:]#]' "$TARGET" 2>/dev/null || exit 0

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
