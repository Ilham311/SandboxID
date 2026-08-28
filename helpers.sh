#!/system/bin/sh

MODDIR="${MODDIR:-/data/adb/modules/sandboxid}"
LOGFILE="${LOGFILE:-/cache/sandboxid-boot.log}"
IDENTITY_FILE="${IDENTITY_FILE:-$MODDIR/identity.prop}"
CARRIER_CONF="${CARRIER_CONF:-$MODDIR/carrier.conf}"
CARRIERS_FILE="${CARRIERS_FILE:-$MODDIR/carriers.tsv}"
BACKUP_DIR_ROOT="${BACKUP_DIR_ROOT:-$MODDIR/backups}"

mkdir -p "$BACKUP_DIR_ROOT" 2>/dev/null
chmod 0700 "$BACKUP_DIR_ROOT" 2>/dev/null


[ -w "$(dirname "$LOGFILE")" ] || LOGFILE="$MODDIR/sandboxid-boot.log"
touch "$LOGFILE" 2>/dev/null

_now() { date '+%Y-%m-%d %H:%M:%S'; }
_log() { printf '[%s] %s\n' "$(_now)" "$*" | tee -a "$LOGFILE" >/dev/null; }
log_step() { _log "==> $*"; }
log_info() { _log "    $*"; }
log_ok()   { _log "[OK] $*"; }
log_warn() { _log "[WARN] $*"; }
log_err()  { _log "[ERR] $*"; }



mask_id() {
    _v="$1"
    [ -z "$_v" ] && { printf '(empty)'; return; }
    _len=${#_v}
    if [ "$_len" -le 6 ]; then
        printf '******'
    else
        printf '%s****%s' "$(printf '%s' "$_v" | cut -c1-4)" "$(printf '%s' "$_v" | cut -c$((_len-1))-)"
    fi
}

_SE_REF=0
_SE_PRIOR=""
se_permissive() {
    if [ "$_SE_REF" -eq 0 ]; then
        _SE_PRIOR="$(getenforce 2>/dev/null || echo Unknown)"
        setenforce 0 2>/dev/null || true
        
        
        trap 'se_restore' EXIT INT TERM HUP
    fi
    _SE_REF=$((_SE_REF + 1))
}
se_restore() {
    [ "$_SE_REF" -gt 0 ] && _SE_REF=$((_SE_REF - 1))
    if [ "$_SE_REF" -eq 0 ] && [ "$_SE_PRIOR" = "Enforcing" ]; then
        setenforce 1 2>/dev/null || true
    fi
}

get_users() {
    if [ -d /data/system/users ]; then
        for d in /data/system/users/[0-9]*; do
            [ -d "$d" ] || continue
            basename "$d"
        done
    else
        echo 0
    fi
}

generate_uuid() {
    if [ -r /proc/sys/kernel/random/uuid ]; then
        cat /proc/sys/kernel/random/uuid
        return
    fi
    r=$(od -An -N16 -tx1 /dev/urandom 2>/dev/null | tr -d ' \n' | cut -c1-32)
    printf '%s-%s-4%s-%s-%s\n' \
        "$(echo "$r" | cut -c1-8)" \
        "$(echo "$r" | cut -c9-12)" \
        "$(echo "$r" | cut -c14-16)" \
        "$(echo "$r" | cut -c17-20)" \
        "$(echo "$r" | cut -c21-32)"
}

generate_mac() {
    b=$(od -An -N5 -tx1 /dev/urandom 2>/dev/null | tr -d ' \n')
    printf '02:%s:%s:%s:%s:%s\n' \
        "$(echo "$b" | cut -c1-2)" \
        "$(echo "$b" | cut -c3-4)" \
        "$(echo "$b" | cut -c5-6)" \
        "$(echo "$b" | cut -c7-8)" \
        "$(echo "$b" | cut -c9-10)"
}

# Framework CLI (settings/am/pm) runner. cmd(1) forwards this shell's std FDs
# to system_server over binder; from action.sh those FDs are a pty/pipe or a
# /data/adb file that SELinux forbids system_server from accessing, so the call
# is rejected with FAILED_TRANSACTION. Pointing all three at /dev/null
# (world-accessible null_device) lets the transaction through. Success is taken
# from the exit code; a short retry covers a genuinely transient busy only.
_fw_run() {
    _n=0
    while [ "$_n" -lt 2 ]; do
        "$@" </dev/null >/dev/null 2>&1 && return 0
        _n=$((_n + 1))
        [ "$_n" -lt 2 ] && sleep 1
    done
    return 1
}

settings_put() {
    scope="$1"; key="$2"; val="$3"
    command -v settings >/dev/null 2>&1 || return 1
    # `settings put <namespace> <key> <value>` — documented platform command
    # (Android `adb shell settings`). --user goes after the verb. See CREDITS.md.
    _fw_run settings put --user 0 "$scope" "$key" "$val"
}

rp_set() {
    key="$1"; val="$2"
    # persist.* props must be written to storage to survive reboot. The -n fast
    # path (set in shared memory, skip property_service) never touches
    # /data/property, so persist keys need -p instead. setprop (final fallback)
    # persists them via property_service anyway. #7g
    case "$key" in
        persist.*) _rpflag="-p" ;;
        *)         _rpflag="-n" ;;
    esac
    if command -v resetprop >/dev/null 2>&1; then
        resetprop "$_rpflag" "$key" "$val" 2>/dev/null && return 0
    fi
    if [ -x "$MODDIR/bin/resetprop-rs" ]; then
        "$MODDIR/bin/resetprop-rs" "$_rpflag" "$key" "$val" 2>/dev/null && return 0
    fi
    if command -v resetprop-rs >/dev/null 2>&1; then
        resetprop-rs "$_rpflag" "$key" "$val" 2>/dev/null && return 0
    fi
    setprop "$key" "$val" 2>/dev/null
}

# Force-stop a package. `am force-stop <pkg>` is the documented primitive that
# stops every process associated with the package; `killall <pkg>` is a
# best-effort sweep for the main process, a technique referenced from
# PlayIntegrityFork's killpi.sh (osm0sis, GPL-3.0). See CREDITS.md.
force_stop() {
    pkg="$1"
    command -v am >/dev/null 2>&1 || return 1
    _fw_run am force-stop --user 0 "$pkg"
    _rc=$?
    command -v killall >/dev/null 2>&1 && killall "$pkg" 2>/dev/null
    return "$_rc"
}

identity_get() {
    key="$1"
    [ -f "$IDENTITY_FILE" ] || return 1
    awk -F= -v k="$key" '$1==k { sub(/^[^=]*=/, ""); print; exit }' "$IDENTITY_FILE" 2>/dev/null
}

identity_persist() {
    key="$1"; val="$2"
    [ -z "$key" ] && return 1
    if [ ! -f "$IDENTITY_FILE" ]; then
        touch "$IDENTITY_FILE" 2>/dev/null || return 1
    fi
    tmp="${IDENTITY_FILE}.tmp.$$"
    awk -F= -v k="$key" '$1!=k {print}' "$IDENTITY_FILE" > "$tmp" 2>/dev/null || { rm -f "$tmp"; return 1; }
    printf '%s=%s\n' "$key" "$val" >> "$tmp"
    mv "$tmp" "$IDENTITY_FILE" 2>/dev/null || { rm -f "$tmp"; return 1; }
    chmod 0644 "$IDENTITY_FILE" 2>/dev/null
    return 0
}

# Remove a key from identity.prop entirely (so the C++ hook finds it absent and
# falls back to the built-in default). Used to clear carrier keys on `carrier off`.
identity_del() {
    key="$1"
    [ -z "$key" ] && return 1
    [ -f "$IDENTITY_FILE" ] || return 0
    tmp="${IDENTITY_FILE}.tmp.$$"
    awk -F= -v k="$key" '$1!=k {print}' "$IDENTITY_FILE" > "$tmp" 2>/dev/null || { rm -f "$tmp"; return 1; }
    mv "$tmp" "$IDENTITY_FILE" 2>/dev/null || { rm -f "$tmp"; return 1; }
    chmod 0644 "$IDENTITY_FILE" 2>/dev/null
    return 0
}

backup_rotate() {
    prefix="$1"; keep="${2:-10}"
    [ -d "$BACKUP_DIR_ROOT" ] || return 0
    ls -1t "$BACKUP_DIR_ROOT"/${prefix}* 2>/dev/null | tail -n +"$((keep + 1))" | while read -r f; do
        rm -f "$f" 2>/dev/null
    done
}

# ────────────────────────────────────────────────────────────────────────────
# ByteDance AppLog SDK cache — did / iid / ssid / openudid / clientudid / cdid
# ────────────────────────────────────────────────────────────────────────────
# What this cleans:
#   AppLog SDK (com.bytedance.applog / com.ss.android.common.applog, aka
#   RangersAppLog / DataFinder) stores server-issued device_id (did), install_id
#   (iid), ssid, openudid, clientudid, cdid in a small set of SharedPreferences
#   XMLs plus a native "device_id" file under files/. On first cold start after
#   these are missing, the SDK regenerates a fresh cdid (UUID v4 from
#   /proc/sys/kernel/random/uuid path in-app) and re-registers with the server
#   via /service/2/device_register/, which mints a new did/iid/ssid pair. So
#   wiping these files is the equivalent of "new install" from the SDK's view.
#
# Companion to `pm clear` in action.sh: `pm clear` already nukes /data/data/<pkg>
# wholesale, but users often want to keep their app data (login, downloads,
# drafts) and only reset the fingerprint. This helper is surgical — it only
# touches AppLog identifier caches, not user data.
#
# Files touched per package (best effort — absent files are fine):
#   shared_prefs/applog.xml                — did / iid / ssid / user_unique_id
#   shared_prefs/applog_stats.xml          — session counters / launch stats
#   shared_prefs/applog_last_sp_session.xml — last session cache
#   shared_prefs/applog_last_data.xml      — last event batch cache
#   shared_prefs/applog_pack.xml           — packing config
#   shared_prefs/snssdk_openudid.xml       — openudid / clientudid
#   shared_prefs/bd_device_info.xml        — device fingerprint blob
#   shared_prefs/snssdk_did.xml            — did cache (older SDKs)
#   shared_prefs/ug_install_settings_pref.xml — install metadata
#   shared_prefs/header_custom.xml         — custom request headers
#   files/bd_setting/device_id             — native did cache (BDTracker)
#   files/applog_v2                        — event queue on-disk
#   files/applog                           — legacy event queue
#   files/.cdid                            — cdid persistence (older builds)
#
# Usage:
#   applog_wipe com.ss.android.ugc.trill
#   applog_wipe               # walk every non-blank line in target.txt
# Returns: 0 if at least one package was processed, 1 if nothing was found.
applog_wipe() {
    _pkg="${1:-}"
    if [ -z "$_pkg" ]; then
        _target="${TARGET_FILE:-$MODDIR/target.txt}"
        if [ ! -r "$_target" ]; then
            log_warn "applog_wipe: target.txt not readable ($_target)"
            return 1
        fi
        _rc=1
        while IFS= read -r _line || [ -n "$_line" ]; do
            _line=${_line%%#*}
            _line=$(printf '%s' "$_line" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
            [ -n "$_line" ] || continue
            applog_wipe "$_line" && _rc=0
        done < "$_target"
        return "$_rc"
    fi

    # Package data dir may live under /data/data or /data/user/0 (symlink on
    # modern Android, but not every device — check both).
    _data_dir=""
    for _base in /data/data /data/user/0; do
        [ -d "$_base/$_pkg" ] && { _data_dir="$_base/$_pkg"; break; }
    done
    if [ -z "$_data_dir" ]; then
        log_info "applog_wipe: $_pkg not installed — skipped"
        return 1
    fi

    log_step "AppLog wipe: $_pkg ($_data_dir)"

    # Only stop when we actually have work to do. Force-stop is a hard signal
    # that will kick the user out of the app; be a good citizen and skip it
    # if the target isn't installed.
    force_stop "$_pkg" >/dev/null 2>&1

    # Snapshot the whole shared_prefs directory into a single dated tarball
    # before we start deleting — one atom, one archive, easy to restore. We
    # skip the tar when there's nothing to snapshot so the backups/ folder
    # doesn't fill up with empty archives.
    se_permissive
    _sp_dir="$_data_dir/shared_prefs"
    if [ -d "$_sp_dir" ]; then
        _ts=$(date +%s)
        _safe_pkg=$(printf '%s' "$_pkg" | tr '/. ' '___')
        _bkp="$BACKUP_DIR_ROOT/applog_${_safe_pkg}_${_ts}.tar"
        # tar is present in Android toybox — cf if any file would be included.
        if ls "$_sp_dir"/applog*.xml "$_sp_dir"/snssdk*.xml \
              "$_sp_dir"/bd_device_info.xml "$_sp_dir"/header_custom.xml \
              "$_sp_dir"/ug_install_settings_pref.xml 2>/dev/null | \
              head -1 | grep -q . ; then
            ( cd "$_sp_dir" && tar -cf "$_bkp" \
                applog*.xml snssdk*.xml bd_device_info.xml \
                header_custom.xml ug_install_settings_pref.xml 2>/dev/null ) || :
            [ -s "$_bkp" ] && chmod 0600 "$_bkp" 2>/dev/null
        fi
    fi

    # ── SharedPreferences layer ────────────────────────────────────────────
    # Explicit list, not a glob-and-hope: we only touch files we recognize as
    # AppLog SDK caches, so user session/login prefs stay put.
    _removed=0
    for _f in \
        applog.xml applog.xml.bak \
        applog_stats.xml applog_stats.xml.bak \
        applog_last_sp_session.xml applog_last_sp_session.xml.bak \
        applog_last_data.xml applog_last_data.xml.bak \
        applog_pack.xml applog_pack.xml.bak \
        applog_easter_egg.xml applog_easter_egg.xml.bak \
        snssdk_openudid.xml snssdk_openudid.xml.bak \
        snssdk_did.xml snssdk_did.xml.bak \
        bd_device_info.xml bd_device_info.xml.bak \
        header_custom.xml header_custom.xml.bak \
        ug_install_settings_pref.xml ug_install_settings_pref.xml.bak
    do
        if [ -e "$_sp_dir/$_f" ]; then
            rm -f "$_sp_dir/$_f" 2>/dev/null && _removed=$((_removed + 1))
        fi
    done

    # ── files/ layer (native SDK persistence) ──────────────────────────────
    # AppLog also writes device_id / event queues outside shared_prefs. These
    # bypass the XML cache and are read directly by the native (libbdtracker*)
    # code, so wiping only XML is not enough for a full "new install" state.
    # Belt-and-braces: only proceed when both the package dir path AND the
    # concrete files/ subdir exist. Guards a hypothetical future refactor
    # where _data_dir could go empty and turn the rm -rf below into `rm -rf
    # /files/...` — shellcheck SC2115 territory.
    _files_dir="$_data_dir/files"
    if [ -n "$_data_dir" ] && [ -d "$_files_dir" ]; then
        for _p in \
            bd_setting/device_id \
            bd_setting/openudid \
            bd_setting/clientudid \
            bd_setting/install_id \
            .cdid \
            applog \
            applog_v2 \
            applog_v3 \
            bd_tracker_n
        do
            # Skip empty/malformed entries defensively.
            [ -n "$_p" ] || continue
            _abs="$_files_dir/$_p"
            # Sanity: full path must live under the package data dir. This
            # blocks any accidental "$_files_dir/" (trailing empty component)
            # from ever hitting rm -rf.
            case "$_abs" in
                "$_data_dir"/files/*) : ;;
                *) continue ;;
            esac
            if [ -e "$_abs" ]; then
                rm -rf "${_abs:?}" 2>/dev/null && _removed=$((_removed + 1))
            fi
        done
    fi

    # ── no_backup/ layer (Android auto-backup exclusion path) ──────────────
    # Newer SDKs stage identifiers under no_backup/ so Google's cloud backup
    # doesn't copy them between devices. Same policy applies to us.
    _nb_dir="$_data_dir/no_backup"
    if [ -d "$_nb_dir" ]; then
        for _p in applog_device_id.dat bd_device_id .cdid; do
            [ -e "$_nb_dir/$_p" ] && rm -f "$_nb_dir/$_p" 2>/dev/null && _removed=$((_removed + 1))
        done
    fi

    se_restore
    backup_rotate "applog_" 20

    if [ "$_removed" -gt 0 ]; then
        log_ok "$_pkg — cleared $_removed AppLog cache entr(y|ies)"
    else
        log_info "$_pkg — no AppLog cache present (already clean)"
    fi
    return 0
}

# ────────────────────────────────────────────────────────────────────────────
# AppLog regenerate — force-stop → wipe → epoch bump (IDs served by the JNI hook)
# ────────────────────────────────────────────────────────────────────────────
# The zygisk module (L9) spoofs did/iid/ssid/openudid/clientudid/cdid in-process:
# every read of shared_prefs/applog*.xml / snssdk_*.xml / bd_device_info.xml is
# patched via memfd redirect, and the raw bd_setting/* + .cdid files are served
# synthesized. Values are deterministic in (persona identity, package name,
# APPLOG_EPOCH from identity.prop) — each ByteDance app gets its own did, like
# the real per-app SDK registration.
#
# Rotation is therefore a two-step affair, both done here:
#   1. bump APPLOG_EPOCH in identity.prop — the next app start derives new IDs
#      (stable until the next bump — no zero-value gap in between);
#   2. wipe the on-disk caches + force-stop, so no stale real did leaks out
#      through backup files or a warm process.
#
# There is deliberately no disk seeding anymore: writing app data files as root
# needs setenforce 0 + chown + restorecon and loses to the SDK's own writes.
# The in-process hook is authoritative — whatever the app writes to disk, the
# next read gets the persona value.
applog_regen() {
    _pkg="${1:-}"
    if [ -z "$_pkg" ]; then
        _target="${TARGET_FILE:-$MODDIR/target.txt}"
        if [ ! -r "$_target" ]; then
            log_warn "applog_regen: target.txt not readable ($_target)"
            return 1
        fi
        _rc=1
        while IFS= read -r _line || [ -n "$_line" ]; do
            _line=${_line%%#*}
            _line=$(printf '%s' "$_line" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
            [ -n "$_line" ] || continue
            applog_regen "$_line" && _rc=0
        done < "$_target"
        return "$_rc"
    fi

    # Bail out cheap if the package isn't even installed — no point stopping
    # or wiping something that doesn't exist.
    _found=0
    for _base in /data/data /data/user/0; do
        [ -d "$_base/$_pkg" ] && { _found=1; break; }
    done
    if [ "$_found" = 0 ]; then
        log_info "applog_regen: $_pkg not installed — skipped"
        return 1
    fi

    log_step "AppLog regen: $_pkg"

    # Fresh epoch = fresh did/iid/ssid/openudid/clientudid/cdid on next start.
    _now_ms="$(date +%s 2>/dev/null || echo 1700000000)000"
    identity_persist APPLOG_EPOCH "$_now_ms" || {
        log_warn "applog_regen: gagal bump APPLOG_EPOCH — ID lama dipakai lagi"
        return 1
    }

    # force_stop first (applog_wipe does it too, but be explicit about the
    # ordering contract), then wipe on-disk caches.
    force_stop "$_pkg" >/dev/null 2>&1
    applog_wipe "$_pkg" || :   # best-effort; missing files are fine
    log_ok "$_pkg — epoch bumped + cache wiped (new IDs on next cold start)"
    return 0
}


# ────────────────────────────────────────────────────────────────────────────
# AppLog probe — read-only status: which cache files currently exist per pkg
# ────────────────────────────────────────────────────────────────────────────
# Emits one line per target: "<pkg> <count> <state>" where state is one of
#   fresh    — 0 cache files exist (post-wipe or never registered)
#   active   — SDK has registered at least once (applog.xml / bd_device_id)
#   absent   — package not installed
# The VALUES on disk are not authoritative anymore — the L9 JNI hook patches
# every read in-process — this probe only reflects on-disk cache presence.
# Never dumps the identifier values themselves. Used by rotate_ids.sh status
# and by the WebUI applog card.
applog_probe() {
    _pkg="${1:-}"
    [ -n "$_pkg" ] || return 1
    _data_dir=""
    for _base in /data/data /data/user/0; do
        [ -d "$_base/$_pkg" ] && { _data_dir="$_base/$_pkg"; break; }
    done
    if [ -z "$_data_dir" ]; then
        printf '%s 0 absent\n' "$_pkg"
        return 0
    fi
    _sp="$_data_dir/shared_prefs"
    _bd="$_data_dir/files/bd_setting"
    _count=0
    for _f in \
        "$_sp/applog.xml" "$_sp/applog_stats.xml" "$_sp/snssdk_openudid.xml" \
        "$_sp/bd_device_info.xml" "$_bd/device_id" "$_bd/install_id" \
        "$_bd/openudid" "$_bd/clientudid" "$_data_dir/files/.cdid"
    do
        [ -e "$_f" ] && _count=$((_count + 1))
    done
    _state=fresh
    # applog.xml on disk means the SDK has run at least once. Note the VALUES
    # in it are irrelevant now — the L9 hook patches them in-process on every
    # read; the file only proves the SDK has initialized (state=active).
    if [ -f "$_sp/applog.xml" ] || [ -f "$_bd/device_id" ]; then
        _state=active
    fi
    printf '%s %d %s\n' "$_pkg" "$_count" "$_state"
    return 0
}
