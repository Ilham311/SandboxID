#!/system/bin/sh

MODDIR="${MODDIR:-/data/adb/modules/sandboxid}"
BIN="${SBX_BIN:-$MODDIR/bin/sandboxid}"
ENABLE_FILE="${AUTOPIF_ENABLE_FILE:-$MODDIR/enable_remote_refresh}"
CONNECT_TIMEOUT="${AUTOPIF_CONNECT_TIMEOUT:-10}"
TOTAL_TIMEOUT="${AUTOPIF_TOTAL_TIMEOUT:-40}"
MAX_BYTES="${AUTOPIF_MAX_BYTES:-262144}"
DOWNLOAD_URL="${AUTOPIF_URL:-https://developer.android.com/about/versions}"
ADAPTER="${AUTOPIF_ADAPTER:-complete-marker}"
TMP_DIR=""

log() { printf '[autopif] %s\n' "$*"; }

resolve_bin() {
    if [ -x "$BIN" ]; then return 0; fi
    if [ -r "$MODDIR/helpers.sh" ]; then
        . "$MODDIR/helpers.sh"
        _resolved="$(sbx_bin 2>/dev/null)"
        [ -x "$_resolved" ] && BIN="$_resolved" && return 0
    fi
    return 1
}

allowed_url() {
    case "$1" in
        https://developer.android.com/*|https://source.android.com/*|https://flash.android.com/*|https://content-flashstation-pa.googleapis.com/*)
            return 0 ;;
        *) return 1 ;;
    esac
}

make_tmp() {
    TMP_DIR="$(mktemp -d "${TMPDIR:-/data/local/tmp}/sbx-autopif.XXXXXX" 2>/dev/null)"
    [ -n "$TMP_DIR" ] && [ -d "$TMP_DIR" ] || {
        TMP_DIR="${TMPDIR:-/data/local/tmp}/sbx-autopif.$$"
        (umask 077 && mkdir "$TMP_DIR") 2>/dev/null || return 1
    }
    chmod 0700 "$TMP_DIR" 2>/dev/null || return 1
    return 0
}

cleanup_tmp() {
    [ -n "$TMP_DIR" ] && rm -rf "$TMP_DIR" 2>/dev/null
    TMP_DIR=""
}

on_signal() {
    _signal_rc="$1"
    cleanup_tmp
    trap - EXIT HUP INT TERM
    exit "$_signal_rc"
}

fetch_url() {
    _url="$1"
    _out="$2"
    allowed_url "$_url" || return 64
    command -v curl >/dev/null 2>&1 || return 127
    curl --fail --silent --show-error \
        --proto '=https' --tlsv1.2 \
        --connect-timeout "$CONNECT_TIMEOUT" --max-time "$TOTAL_TIMEOUT" \
        --max-filesize "$MAX_BYTES" \
        --header 'Accept: text/plain,text/html,application/json;q=0.9,*/*;q=0.1' \
        --output "$_out" "$_url" 2>/dev/null
    _curl_rc=$?
    [ "$_curl_rc" -eq 0 ] || return "$_curl_rc"
    _size="$(wc -c < "$_out" 2>/dev/null | tr -d ' ')"
    case "$_size" in ''|*[!0-9]*) return 1 ;; esac
    [ "$_size" -gt 0 ] && [ "$_size" -le "$MAX_BYTES" ]
}

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v toybox >/dev/null 2>&1; then
        toybox sha256sum "$1" | awk '{print $1}'
    else
        return 1
    fi
}

parse_candidate() {
    _input="$1"
    _out="$2"
    case "$ADAPTER" in
        complete-marker) : ;;
        *) return 1 ;;
    esac

    # The network source must publish one complete persona-v1 row explicitly.
    # Human release pages and undocumented endpoints are not scraped or guessed:
    # missing fields, including SECURITY_PATCH, always reject the candidate.
    if [ -n "${AUTOPIF_CANDIDATE_FILE:-}" ]; then
        [ "${AUTOPIF_TEST_MODE:-0}" = 1 ] || return 1
        [ -r "$AUTOPIF_CANDIDATE_FILE" ] || return 1
        cp "$AUTOPIF_CANDIDATE_FILE" "$_out" 2>/dev/null || return 1
    else
        awk '
            /^SBX_PERSONA_V1[[:space:]]/ {
                sub(/^SBX_PERSONA_V1[[:space:]]+/, "")
                print
                found++
            }
            END { if (found != 1) exit 1 }
        ' "$_input" > "$_out" 2>/dev/null || return 1
    fi
    [ -s "$_out" ] || return 1
    _lines="$(wc -l < "$_out" 2>/dev/null | tr -d ' ')"
    [ "$_lines" = 1 ] || return 1
    _bytes="$(wc -c < "$_out" 2>/dev/null | tr -d ' ')"
    case "$_bytes" in ''|*[!0-9]*) return 1 ;; esac
    [ "$_bytes" -le 4096 ]
}

cmd_refresh() {
    [ -f "$ENABLE_FILE" ] || {
        log "refresh nonaktif; buat $ENABLE_FILE untuk mengizinkan jaringan"
        return 0
    }
    resolve_bin || {
        log "binary native tidak tersedia; cache lama dipertahankan"
        return 127
    }
    allowed_url "$DOWNLOAD_URL" || {
        log "URL ditolak; hanya HTTPS host Google yang diizinkan"
        return 64
    }
    _sdk="${SBX_REAL_SDK:-$(getprop ro.build.version.sdk 2>/dev/null)}"
    case "$_sdk" in ''|*[!0-9]*)
        log "SDK runtime tidak tersedia; cache lama dipertahankan"
        return 64 ;;
    esac
    make_tmp || {
        log "direktori sementara privat tidak tersedia"
        return 1
    }
    trap cleanup_tmp EXIT
    trap 'on_signal 129' HUP
    trap 'on_signal 130' INT
    trap 'on_signal 143' TERM
    _raw="$TMP_DIR/remote"
    _candidate="$TMP_DIR/candidate.tsv"
    _meta="$TMP_DIR/candidate.meta"
    fetch_url "$DOWNLOAD_URL" "$_raw"
    _rc=$?
    if [ "$_rc" -ne 0 ]; then
        log "unduhan gagal (rc=$_rc); cache lama dipertahankan"
        return "$_rc"
    fi
    if ! parse_candidate "$_raw" "$_candidate"; then
        log "adapter tidak menemukan satu persona lengkap; cache lama dipertahankan"
        return 1
    fi
    _hash="$(sha256_file "$_candidate" 2>/dev/null)"
    if [ "${#_hash}" -ne 64 ]; then
        log "SHA-256 tidak tersedia; cache lama dipertahankan"
        return 1
    fi
    case "$_hash" in
        *[!0-9a-f]*) log "SHA-256 tidak tersedia; cache lama dipertahankan"; return 1 ;;
    esac
    _retrieved="$(date +%s 2>/dev/null)"
    case "$_retrieved" in ''|*[!0-9]*|0) return 1 ;; esac
    {
        printf 'version=1\n'
        printf 'schema=persona-v1\n'
        printf 'parser=autopif-v1\n'
        printf 'retrieved_utc=%s\n' "$_retrieved"
        printf 'runtime_sdk=%s\n' "$_sdk"
        printf 'candidate_sha256=%s\n' "$_hash"
        printf 'adapter=%s\n' "$ADAPTER"
        printf 'url=%s\n' "$DOWNLOAD_URL"
    } > "$_meta" || return 1
    if "$BIN" persona-import "$_candidate" "$_meta"; then
        log "persona exact-SDK tervalidasi dan cache diperbarui"
        return 0
    fi
    _rc=$?
    log "validasi/import native gagal (rc=$_rc); cache lama dipertahankan"
    return "$_rc"
}

case "${1:-refresh}" in
    refresh|'') cmd_refresh ;;
    -h|--help|help)
        printf 'Usage: autopif.sh refresh\n'
        printf 'Remote refresh is opt-in via %s.\n' "$ENABLE_FILE"
        exit 0 ;;
    *)
        log "subcommand tidak dikenal; gunakan: refresh"
        exit 64 ;;
esac
exit $?
