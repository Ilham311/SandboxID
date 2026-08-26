#!/system/bin/sh
#
# selftest.sh — READ-ONLY detection self-check (F5).
#
# Scans the device from a root shell and REPORTS what a detector would find. It
# changes NOTHING: only getprop / cat / ls / command -v / mountinfo parsing. No
# setprop, no resetprop, no setenforce, no mount, no framework CLIs.
#
# Vantage point matters, and it is the whole reason for the PASS/WARN/FAIL/INFO
# split:
#   * Device-wide properties set by `apply_native` (resetprop) ARE visible to
#     this root shell via getprop, so those are graded PASS / WARN / FAIL.
#   * Per-app effects — the L2/L9 property + Build hooks, the /proc,/sys memfd
#     redirects, the per-app build.prop bind-mounts, and the opt-in mount-trace
#     hider — live in each TARGET app's own hook + mount namespace. A root shell
#     cannot see them, so they are reported INFO ("verify inside a target app")
#     and must NEVER read as a false FAIL.
#   * Real, device-global root traces (su, manager dirs, magisk/overlay mounts)
#     are equally invisible to a normal app unless it is a target with the hider
#     enabled, so those are INFO too — this shell always sees them.
#
# Oracle for the check list: reveny/Android-Native-Root-Detector's documented
# signal categories (MIT shell; the detector .so is closed and was NOT copied —
# used only as a requirements checklist). See CREDITS.md.
#
# Output grammar (parse-token contract with webroot/app.js — keep in sync with
# .gitar/documents/parse-token-safelist.md):
#     SELFTEST <category> <PASS|WARN|FAIL|INFO> <detail...>
#     SELFTEST SUMMARY pass=N warn=M fail=K info=J
# category is a single space-free token; detail is free text.

MODDIR="${MODDIR:-${0%/*}}"
IDENTITY="${IDENTITY:-$MODDIR/identity.prop}"

PASS=0; WARN=0; FAIL=0; INFO=0

emit() {
    case "$2" in
        PASS) PASS=$((PASS + 1)) ;;
        WARN) WARN=$((WARN + 1)) ;;
        FAIL) FAIL=$((FAIL + 1)) ;;
        INFO) INFO=$((INFO + 1)) ;;
    esac
    printf 'SELFTEST %s %s %s\n' "$1" "$2" "$3"
}

gp() { getprop "$1" 2>/dev/null; }

id_get() {
    [ -f "$IDENTITY" ] || return 1
    awk -F= -v k="$1" '$1==k { sub(/^[^=]*=/, ""); print; exit }' "$IDENTITY" 2>/dev/null
}

# --- identity: is a persona applied at all? -------------------------------
_fp="$(id_get FINGERPRINT)"
if [ -n "$_fp" ]; then
    emit identitas PASS "persona aktif: $_fp"
else
    emit identitas WARN "belum ada persona — tekan Acak perangkat baru"
fi

# --- verified boot / VBMeta (device-wide, set by apply_native) ------------
_vbs="$(gp ro.boot.verifiedbootstate)"
case "$_vbs" in
    green)             emit vbmeta PASS "verifiedbootstate=green" ;;
    orange|yellow|red) emit vbmeta FAIL "verifiedbootstate=$_vbs (bootloader tidak terkunci/termodifikasi)" ;;
    "")                emit vbmeta WARN "verifiedbootstate kosong — apply-boot belum jalan?" ;;
    *)                 emit vbmeta WARN "verifiedbootstate=$_vbs (tak dikenal)" ;;
esac

_ds="$(gp ro.boot.vbmeta.device_state)"
case "$_ds" in
    locked)   emit vbmeta PASS "vbmeta.device_state=locked" ;;
    unlocked) emit vbmeta FAIL "vbmeta.device_state=unlocked" ;;
    "")       emit vbmeta WARN "vbmeta.device_state kosong" ;;
    *)        emit vbmeta WARN "vbmeta.device_state=$_ds" ;;
esac

_fl="$(gp ro.boot.flash.locked)"
case "$_fl" in
    1)  emit vbmeta PASS "flash.locked=1" ;;
    0)  emit vbmeta FAIL "flash.locked=0 (bootloader terbuka)" ;;
    "") emit vbmeta WARN "flash.locked kosong" ;;
    *)  emit vbmeta WARN "flash.locked=$_fl" ;;
esac

_vm="$(gp ro.boot.veritymode)"
case "$_vm" in
    enforcing) emit vbmeta PASS "veritymode=enforcing" ;;
    "")        emit vbmeta WARN "veritymode kosong" ;;
    *)         emit vbmeta WARN "veritymode=$_vm (bukan enforcing)" ;;
esac

_dig="$(gp ro.boot.vbmeta.digest)"
case "$_dig" in
    "") emit vbmeta WARN "vbmeta.digest kosong" ;;
    *)
        # 64 lowercase hex = well-formed SHA-256; anything else is suspicious.
        _clean="$(printf '%s' "$_dig" | tr -d '0-9a-f')"
        if [ -z "$_clean" ] && [ "${#_dig}" -eq 64 ]; then
            emit vbmeta PASS "vbmeta.digest terpasang (64-hex)"
        else
            emit vbmeta WARN "vbmeta.digest bentuk tak lazim (len=${#_dig})"
        fi
        ;;
esac

# --- build.prop identity tells (device-wide) ------------------------------
_bt="$(gp ro.build.type)"
case "$_bt" in
    user)          emit build PASS "ro.build.type=user" ;;
    userdebug|eng) emit build FAIL "ro.build.type=$_bt (build non-rilis)" ;;
    "")            emit build WARN "ro.build.type kosong" ;;
    *)             emit build WARN "ro.build.type=$_bt" ;;
esac

_tg="$(gp ro.build.tags)"
case "$_tg" in
    release-keys) emit build PASS "ro.build.tags=release-keys" ;;
    *test-keys*)  emit build FAIL "ro.build.tags=$_tg (test-keys)" ;;
    "")           emit build WARN "ro.build.tags kosong" ;;
    *)            emit build WARN "ro.build.tags=$_tg" ;;
esac

_dbg="$(gp ro.debuggable)"
case "$_dbg" in
    0)  emit build PASS "ro.debuggable=0" ;;
    1)  emit build FAIL "ro.debuggable=1 (debuggable)" ;;
    *)  emit build WARN "ro.debuggable=${_dbg:-kosong}" ;;
esac

_sec="$(gp ro.secure)"
case "$_sec" in
    1)  emit build PASS "ro.secure=1" ;;
    0)  emit build FAIL "ro.secure=0" ;;
    *)  emit build WARN "ro.secure=${_sec:-kosong}" ;;
esac

_oem="$(gp sys.oem_unlock_allowed)"
case "$_oem" in
    0)  emit build PASS "sys.oem_unlock_allowed=0" ;;
    1)  emit build WARN "sys.oem_unlock_allowed=1 (OEM unlock diizinkan)" ;;
    *)  emit build INFO "sys.oem_unlock_allowed=${_oem:-kosong}" ;;
esac

# --- SELinux (global; the /sys/fs/selinux/enforce mask is per-target-app) --
_se="$(getenforce 2>/dev/null)"
case "$_se" in
    Enforcing)  emit selinux PASS "getenforce=Enforcing" ;;
    Permissive) emit selinux WARN "getenforce=Permissive (tell global; modul tidak mengubahnya)" ;;
    "")         emit selinux INFO "getenforce tidak tersedia" ;;
    *)          emit selinux INFO "getenforce=$_se" ;;
esac
emit selinux INFO "ro.build.selinux + node enforce di-mask per-app — verifikasi di dalam app target"

# --- emulator / custom-ROM property tells (device-wide) -------------------
_qemu="$(gp ro.kernel.qemu)$(gp ro.boot.qemu)"
_hw="$(gp ro.hardware)"
if [ -n "$_qemu" ] || [ "$_hw" = "goldfish" ] || [ "$_hw" = "ranchu" ]; then
    emit rom FAIL "prop emulator/qemu terdeteksi device-wide (hw=$_hw)"
else
    emit rom PASS "tidak ada prop qemu device-wide"
fi

_lin="$(gp ro.modversion)$(gp ro.lineage.version)$(gp ro.cm.version)"
if [ -n "$_lin" ]; then
    emit rom WARN "prop custom-ROM device-wide ada (di-mask per-app oleh hook)"
else
    emit rom PASS "tidak ada prop custom-ROM device-wide"
fi

# --- real root traces: visible to THIS shell; hidden from an app only when
#     it is a target AND the opt-in hider is enabled. Hence INFO, never FAIL. -
if command -v su >/dev/null 2>&1; then
    emit root INFO "biner su terlihat dari root shell (disembunyikan di app target hanya bila hider aktif)"
else
    emit root INFO "su tidak di PATH shell ini"
fi

_mgr=""
[ -d /data/adb/magisk ]      && _mgr="$_mgr magisk"
[ -d /data/adb/ksu ] || [ -e /data/adb/ksud ] && _mgr="$_mgr kernelsu"
[ -d /data/adb/ap ]  || [ -d /data/adb/apatch ] && _mgr="$_mgr apatch"
_mgr="${_mgr# }"
emit root INFO "root solution: ${_mgr:-tidak terdeteksi di /data/adb}"

if [ -f "$MODDIR/enable_hide" ]; then
    emit root INFO "hider mount-trace: AKTIF (opt-in) — berlaku di dalam app target"
else
    emit root INFO "hider mount-trace: nonaktif (default) — touch enable_hide untuk mengaktifkan"
fi

# --- mount traces in the ROOT namespace (an app target sees its OWN ns) ----
_mi=/proc/self/mountinfo
if [ -r "$_mi" ]; then
    _n="$(grep -c -E 'magisk|/data/adb|debug_ramdisk|worker| overlay | tmpfs ' "$_mi" 2>/dev/null)"
    [ -n "$_n" ] || _n=0
    emit mount INFO "$_n baris mount mencurigakan di root ns (di app target di-detach hanya bila hider aktif)"
else
    emit mount INFO "mountinfo tidak terbaca"
fi

# --- modified hosts (adblock/hosts tell) ----------------------------------
if [ -r /system/etc/hosts ]; then
    _hn="$(grep -c -v -E '^[[:space:]]*(#|$)' /system/etc/hosts 2>/dev/null)"
    [ -n "$_hn" ] || _hn=0
    if [ "$_hn" -gt 10 ]; then
        emit hosts WARN "/system/etc/hosts punya $_hn entri (kemungkinan hosts adblock)"
    else
        emit hosts PASS "/system/etc/hosts wajar ($_hn entri)"
    fi
else
    emit hosts INFO "/system/etc/hosts tidak terbaca"
fi

# --- per-app hook layers: only observable from inside a target app ---------
_nnr="nonaktif"; [ -f "$MODDIR/no_native_read" ] && _nnr="AKTIF (native-read dimatikan)"
emit hooks INFO "redirect baca /proc,/sys (boot_id, MAC, /proc/version, meminfo, cpuinfo, enforce): kill-switch no_native_read=$_nnr"
emit hooks INFO "hook properti L2/L9 + bind build.prop: hanya di app target — verifikasi dengan app detektor"

printf 'SELFTEST SUMMARY pass=%d warn=%d fail=%d info=%d\n' "$PASS" "$WARN" "$FAIL" "$INFO"
