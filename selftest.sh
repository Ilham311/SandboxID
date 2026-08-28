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

# --- coherence self-audit (J): internal consistency of the applied persona -
# All fields come from identity.prop. A detector that RECOMPUTES the fingerprint
# from Build.* or MAPS SDK->release will flag any mismatch, and an SDK/RELEASE
# skew is exactly the "upgrade spoof" that force-closes target apps. Catching it
# here means a broken persona is spotted before a target app ever sees it. These
# are read-only string checks on identity.prop and run only when a persona is
# applied (otherwise the identitas WARN above already covers the empty case).
if [ -n "$_fp" ]; then
    _brand="$(id_get BRAND)";     _product="$(id_get PRODUCT)"
    _device="$(id_get DEVICE)";   _release="$(id_get RELEASE)"
    _bid="$(id_get ID)";          _incr="$(id_get INCREMENTAL)"
    _sdk="$(id_get SDK_INT)";     _patch="$(id_get SECURITY_PATCH)"
    _plat="$(id_get BOARD_PLATFORM)"
    _socman="$(id_get SOC_MANUFACTURER)"; _socmod="$(id_get SOC_MODEL)"
    _type="$(id_get TYPE)";       _tags="$(id_get TAGS)"

    # 1. FINGERPRINT == BRAND/PRODUCT/DEVICE:RELEASE/ID/INCREMENTAL:user/release-keys
    _expfp="${_brand}/${_product}/${_device}:${_release}/${_bid}/${_incr}:user/release-keys"
    if [ "$_fp" = "$_expfp" ]; then
        emit koherensi PASS "fingerprint konsisten dengan field Build"
    else
        emit koherensi FAIL "fingerprint tidak cocok dengan BRAND/PRODUCT/DEVICE/RELEASE/ID/INCREMENTAL"
    fi

    # 2. SDK_INT <-> RELEASE major (upgrade-spoof / API-mismatch guard)
    case "$_sdk" in
        30) _exprel=11 ;;
        31) _exprel=12 ;;
        32) _exprel=12 ;;   # 12L reports release "12"
        33) _exprel=13 ;;
        34) _exprel=14 ;;
        35) _exprel=15 ;;
        36) _exprel=16 ;;
        *)  _exprel="" ;;
    esac
    _relmaj="${_release%%.*}"
    if [ -z "$_sdk" ]; then
        emit koherensi WARN "SDK_INT kosong"
    elif [ -z "$_exprel" ]; then
        emit koherensi WARN "SDK_INT=$_sdk di luar rentang yang dipetakan (30-36)"
    elif [ "$_relmaj" = "$_exprel" ]; then
        emit koherensi PASS "SDK_INT=$_sdk cocok dengan RELEASE=$_release"
    else
        emit koherensi FAIL "SDK_INT=$_sdk tapi RELEASE=$_release (harusnya Android $_exprel)"
    fi

    # 3. SECURITY_PATCH must be YYYY-MM-DD (10 chars; digits removed leaves "--")
    _pd="$(printf '%s' "$_patch" | tr -d '0-9')"
    if [ "${#_patch}" -eq 10 ] && [ "$_pd" = "--" ]; then
        emit koherensi PASS "security_patch berformat YYYY-MM-DD ($_patch)"
    else
        emit koherensi WARN "security_patch bentuk tak lazim (${_patch:-kosong})"
    fi

    # 4. TYPE=user / TAGS=release-keys (retail tells; must match fingerprint tail)
    if [ "$_type" = "user" ] && [ "$_tags" = "release-keys" ]; then
        emit koherensi PASS "TYPE=user TAGS=release-keys"
    else
        emit koherensi WARN "TYPE=${_type:-kosong} TAGS=${_tags:-kosong} (bukan user/release-keys)"
    fi

    # 5. board platform <-> SoC coherence (SoC-leak guard). A Tensor platform
    #    must report a Google GS* SoC; a non-Tensor row must carry a SoC model.
    case "$_plat" in
        gs101|gs201|zuma|zumapro|laguna)
            case "$_socmod" in
                GS[0-9][0-9][0-9])
                    if [ "$_socman" = "Google" ]; then
                        emit koherensi PASS "SoC Tensor konsisten ($_socman $_socmod / $_plat)"
                    else
                        emit koherensi WARN "platform Tensor $_plat tapi SOC_MANUFACTURER=$_socman (bukan Google)"
                    fi
                    ;;
                *) emit koherensi FAIL "platform Tensor $_plat tapi SOC_MODEL=${_socmod:-kosong} (bukan GS*)" ;;
            esac
            ;;
        "") emit koherensi INFO "BOARD_PLATFORM kosong — lewati cek SoC" ;;
        *)
            if [ -n "$_socmod" ]; then
                emit koherensi PASS "SoC non-Tensor terisi ($_socman $_socmod / $_plat)"
            else
                emit koherensi WARN "SOC_MODEL kosong untuk platform $_plat"
            fi
            ;;
    esac

    # 6. FLAVOR must equal PRODUCT-TYPE ("oriole-user"); detectors compare it
    #    against the fingerprint tail.
    _flavor="$(id_get FLAVOR)"
    if [ -n "$_flavor" ]; then
        if [ "$_flavor" = "${_product}-${_type}" ]; then
            emit koherensi PASS "FLAVOR konsisten ($_flavor)"
        else
            emit koherensi FAIL "FLAVOR='$_flavor' != PRODUCT-TYPE (${_product}-${_type})"
        fi
    fi

    # 7. BUILD_TIME_UTC (Build.TIME / ro.build.date.utc) — numeric and within
    #    a plausible window: after 2009-01-01, before now.
    _butc="$(id_get BUILD_TIME_UTC)"
    _nowts="$(date +%s 2>/dev/null)"
    case "$_butc" in
        '') emit koherensi WARN "BUILD_TIME_UTC kosong — Build.Time tidak dispoof (persona lama?)" ;;
        *[!0-9]*)
            emit koherensi FAIL "BUILD_TIME_UTC bukan angka ($_butc)" ;;
        *)
            if [ -n "$_nowts" ] && [ "$_butc" -ge 1230768000 ] && [ "$_butc" -le "$_nowts" ]; then
                emit koherensi PASS "BUILD_TIME_UTC masuk rentang wajar ($_butc)"
            else
                emit koherensi FAIL "BUILD_TIME_UTC di luar rentang wajar ($_butc)"
            fi
            ;;
    esac
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
