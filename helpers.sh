#!/system/bin/sh

MODDIR="${MODDIR:-/data/adb/modules/ternak_tt}"
LOGFILE="${LOGFILE:-/cache/ternak-tt-boot.log}"
IDENTITY_FILE="${IDENTITY_FILE:-$MODDIR/identity.prop}"
BACKUP_DIR_ROOT="${BACKUP_DIR_ROOT:-$MODDIR/backups}"

mkdir -p "$BACKUP_DIR_ROOT" 2>/dev/null
chmod 0700 "$BACKUP_DIR_ROOT" 2>/dev/null
[ -w "$(dirname "$LOGFILE")" ] || LOGFILE=/data/local/tmp/ternak-tt-boot.log
touch "$LOGFILE" 2>/dev/null

_now() { date '+%Y-%m-%d %H:%M:%S'; }
_log() { printf '[%s] %s\n' "$(_now)" "$*" | tee -a "$LOGFILE" >/dev/null; }
log_step() { _log "==> $*"; }
log_info() { _log "    $*"; }
log_ok()   { _log "[OK] $*"; }
log_warn() { _log "[WARN] $*"; }
log_err()  { _log "[ERR] $*"; }

_SE_REF=0
_SE_PRIOR=""
se_permissive() {
    if [ "$_SE_REF" -eq 0 ]; then
        _SE_PRIOR="$(getenforce 2>/dev/null || echo Unknown)"
        setenforce 0 2>/dev/null || true
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

settings_put() {
    scope="$1"; key="$2"; val="$3"
    command -v settings >/dev/null 2>&1 || return 1
    settings put "$scope" "$key" "$val" 2>/dev/null
}

rp_set() {
    key="$1"; val="$2"
    if command -v resetprop >/dev/null 2>&1; then
        resetprop -n "$key" "$val" 2>/dev/null && return 0
    fi
    if [ -x "$MODDIR/bin/resetprop-rs" ]; then
        "$MODDIR/bin/resetprop-rs" -n "$key" "$val" 2>/dev/null && return 0
    fi
    if command -v resetprop-rs >/dev/null 2>&1; then
        resetprop-rs -n "$key" "$val" 2>/dev/null && return 0
    fi
    setprop "$key" "$val" 2>/dev/null
}

force_stop() {
    pkg="$1"
    command -v am >/dev/null 2>&1 || return 1
    am force-stop "$pkg" 2>/dev/null
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

backup_rotate() {
    prefix="$1"; keep="${2:-10}"
    [ -d "$BACKUP_DIR_ROOT" ] || return 0
    ls -1t "$BACKUP_DIR_ROOT"/${prefix}* 2>/dev/null | tail -n +"$((keep + 1))" | while read -r f; do
        rm -f "$f" 2>/dev/null
    done
}
