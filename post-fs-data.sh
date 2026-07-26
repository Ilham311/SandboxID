#!/system/bin/sh
# ============================================================
# Ternak TT — post-fs-data.sh (v1.0.14)
#
# Runs VERY early during boot, before Zygote/SystemServer start.
# Purpose: guarantee the mount overlay tree exists before ANY target
# TikTok/Grab process can spawn, so the first companion bind-mount
# request succeeds (v1.0.12 log showed 6-skip on first pid=8361 because
# service.sh hadn't run apply-boot yet).
#
# `ternak-tt seed` is intentionally fast + minimal:
#   * writes identity.prop if missing (reuses existing otherwise)
#   * regenerates mount/*/build.prop and settings_secure.xml
#   * does NOT call resetprop-rs or pm (those binaries need Android up)
# service.sh runs apply-boot later once sys.boot_completed=1 for the
# native prop broadcast + TT data wipe.
# ============================================================
MODDIR="${0%/*}"

# The binary symlink is arch-specific and set up by customize.sh's
# ln -sf logic. If the symlink is missing (unusual), fall back to the
# arm64 build (this device is arm64-v8a).
BIN="$MODDIR/bin/ternak-tt"
[ -x "$BIN" ] || BIN="$MODDIR/bin/ternak-tt-arm64"

if [ -x "$BIN" ]; then
    "$BIN" seed >> /cache/ternak-tt-boot.log 2>&1
fi
