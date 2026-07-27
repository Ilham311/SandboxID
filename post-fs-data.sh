#!/system/bin/sh
# Ternak TT post-fs-data — runs BEFORE zygote starts.
# v1.2.4: also refresh $MODDIR/system.prop so Magisk applies build.prop overrides
#         to the property_service BEFORE zygote caches Build.* static fields
#         (takes effect on the NEXT boot).
MODDIR="${0%/*}"

BIN="$MODDIR/bin/ternak-tt"
[ -x "$BIN" ] || BIN="$MODDIR/bin/ternak-tt-arm64"

if [ -x "$BIN" ]; then
    "$BIN" seed >> /cache/ternak-tt-boot.log 2>&1
fi

# --- v1.2.4: regenerate system.prop for NEXT boot -----------------------------
# Magisk reads $MODDIR/system.prop at early-init and applies each key=value via
# resetprop BEFORE zygote starts. That is the only reliable way to spoof
# android.os.Build.* static fields, because zygote caches them at first read.
# This block runs every boot; effect lands on the SUBSEQUENT boot.
SYSPROP="$MODDIR/system.prop"
SYSPROP_TMP="$MODDIR/.system.prop.tmp"
{
    echo "# Ternak TT v1.2.4 auto-generated system.prop"
    echo "# Applied by Magisk before zygote starts"
    for F in "$MODDIR/mount/system/build.prop" \
             "$MODDIR/mount/vendor/build.prop" \
             "$MODDIR/mount/odm/build.prop" \
             "$MODDIR/mount/product/build.prop" \
             "$MODDIR/mount/system_ext/build.prop"; do
        [ -f "$F" ] && grep -E '^[a-zA-Z][a-zA-Z0-9._]*=' "$F"
    done
} | awk '!seen[$0]++' > "$SYSPROP_TMP" 2>> /cache/ternak-tt-boot.log

if [ -s "$SYSPROP_TMP" ]; then
    mv -f "$SYSPROP_TMP" "$SYSPROP"
    chmod 0644 "$SYSPROP"
    LINES=$(wc -l < "$SYSPROP" 2>/dev/null || echo 0)
    echo "OK: system.prop refreshed ($LINES lines) — effective next boot" \
        >> /cache/ternak-tt-boot.log
else
    rm -f "$SYSPROP_TMP"
    echo "! system.prop refresh skipped (mount/*/build.prop empty)" \
        >> /cache/ternak-tt-boot.log
fi
