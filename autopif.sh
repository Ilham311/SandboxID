#!/system/bin/sh

# Pixel OTA discovery and partial-metadata extraction are adapted from
# JingMatrix/PlayIntegrityFix@1b7ecc1b931498cb6cc3ca16433c39444631bf33
# (module/action.sh). Randomized traversal follows the same project's
# 124d61f55277351f4eb24737b8c96d8ed6320036. The strict bounds, exact-runtime
# validation, Pixel platform map, prior-device exclusion, provenance, and native
# publication below are SandboxID adapters around those upstream patterns.
MODDIR="${MODDIR:-/data/adb/modules/sandboxid}"
BIN="${SBX_BIN:-$MODDIR/bin/sandboxid}"
CACHE_FILE="${PERSONA_CACHE:-$MODDIR/persona.cache}"
CACHE_META="${PERSONA_CACHE_META:-$MODDIR/persona.cache.meta}"
CONNECT_TIMEOUT="${AUTOPIF_CONNECT_TIMEOUT:-10}"
TOTAL_TIMEOUT="${AUTOPIF_TOTAL_TIMEOUT:-40}"
CATALOG_MAX_BYTES="${AUTOPIF_CATALOG_MAX_BYTES:-524288}"
OTA_MAX_BYTES="${AUTOPIF_OTA_MAX_BYTES:-524288}"
MAX_TRIES="${AUTOPIF_MAX_TRIES:-8}"
ADAPTER="${AUTOPIF_ADAPTER:-pixel-ota-v1}"
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
        https://developer.android.com/about/versions/15/download-ota|\
        https://developer.android.com/about/versions/16/download-ota)
            return 0 ;;
        https://dl.google.com/developers/android/vic/images/ota/*)
            _ota_name="${1#https://dl.google.com/developers/android/vic/images/ota/}"
            case "$_ota_name" in
                ''|*[!A-Za-z0-9._+-]*) return 1 ;;
                *_beta-ota-*.zip) return 0 ;;
                *) return 1 ;;
            esac ;;
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

valid_bound() {
    case "$1" in ''|*[!0-9]*|0) return 1 ;; esac
    [ "$1" -le 8388608 ]
}

fetch_url() {
    _url="$1"
    _out="$2"
    _max="$3"
    _range="${4:-}"
    allowed_url "$_url" || return 64
    valid_bound "$_max" || return 64
    command -v curl >/dev/null 2>&1 || return 127
    set -- curl --fail --silent --show-error \
        --proto '=https' --tlsv1.2 \
        --connect-timeout "$CONNECT_TIMEOUT" --max-time "$TOTAL_TIMEOUT" \
        --max-filesize "$_max" \
        --header 'Accept: text/html,application/zip;q=0.9,*/*;q=0.1'
    [ -z "$_range" ] || set -- "$@" --range "$_range"
    "$@" --output "$_out" "$_url" 2>/dev/null
    _curl_rc=$?
    [ "$_curl_rc" -eq 0 ] || return "$_curl_rc"
    _size="$(wc -c < "$_out" 2>/dev/null | tr -d ' ')"
    case "$_size" in ''|*[!0-9]*) return 1 ;; esac
    [ "$_size" -gt 0 ] && [ "$_size" -le "$_max" ]
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

sdk_release() {
    case "$1" in
        35) printf '15\n' ;;
        36) printf '16\n' ;;
        *) return 1 ;;
    esac
}

map_platform() {
    case "$1" in
        oriole|raven) echo 'gs101' ;;
        panther|cheetah|lynx|felix|tangorpro) echo 'gs201' ;;
        shiba|husky|akita) echo 'zuma' ;;
        tokay|caiman|komodo|comet) echo 'zumapro' ;;
        *) return 1 ;;
    esac
}

soc_model() {
    case "$1" in
        gs101) echo 'GS101' ;;
        gs201) echo 'GS201' ;;
        zuma) echo 'GS301' ;;
        zumapro) echo 'GS401' ;;
        laguna) echo 'GS501' ;;
        *) return 1 ;;
    esac
}

rand_below() {
    _limit="$1"
    [ "$_limit" -gt 0 ] 2>/dev/null || return 1
    if [ -n "${AUTOPIF_RANDOM:-}" ]; then
        _random="$AUTOPIF_RANDOM"
    else
        _random="$(od -An -N2 -tu2 /dev/urandom 2>/dev/null | tr -d ' ')"
    fi
    case "$_random" in ''|*[!0-9]*) _random=$$ ;; esac
    printf '%s\n' $(( _random % _limit ))
}

prior_device() {
    _runtime_sdk="$1"
    [ -r "$CACHE_FILE" ] && [ -r "$CACHE_META" ] || return 0
    _expected_hash="$(awk -F= -v sdk="$_runtime_sdk" '
        {
            key=$1
            value=substr($0, length(key) + 2)
            if (key == "" || ++seen[key] != 1) bad=1
            count++
            if (key == "version" && value == "1") version=1
            else if (key == "schema" && value == "persona-v1") schema=1
            else if (key == "parser" && value == "autopif-v1") parser=1
            else if (key == "retrieved_utc" && value ~ /^[1-9][0-9]*$/) retrieved=1
            else if (key == "runtime_sdk" && value == sdk) runtime=1
            else if (key == "candidate_sha256") hash=value
            else if (key == "adapter" && value == "pixel-ota-v1") adapter=1
            else if (key == "url" && index(value, "https://dl.google.com/developers/android/vic/images/ota/") == 1 && value ~ /_beta-ota-.*\.zip$/) url=1
            else bad=1
        }
        END {
            if (!bad && count == 8 && version && schema && parser && retrieved &&
                runtime && adapter && url) print hash
        }
    ' "$CACHE_META" 2>/dev/null)"
    case "$_expected_hash" in
        [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]*) : ;;
        *) return 0 ;;
    esac
    [ "${#_expected_hash}" -eq 64 ] || return 0
    _actual_hash="$(sha256_file "$CACHE_FILE" 2>/dev/null)"
    [ "$_actual_hash" = "$_expected_hash" ] || return 0
    awk -F '\t' -v sdk="$_runtime_sdk" '
        NR == 1 && NF == 16 && $6 == sdk && $2 ~ /^[a-z0-9_]+$/ { device=$2 }
        END { if (NR == 1 && device != "") print device }
    ' "$CACHE_FILE" 2>/dev/null
}

extract_catalog() {
    _input="$1"
    _output="$2"
    awk '
        /<tr id="[a-z0-9_]+"/ {
            line=$0
            sub(/^.*<tr id="/, "", line)
            sub(/".*$/, "", line)
            table_device=line
            next
        }
        table_device != "" && /<td>[^<]+<\/td>/ {
            line=$0
            sub(/^.*<td>/, "", line)
            sub(/<\/td>.*$/, "", line)
            models[table_device]=line
            table_device=""
            next
        }
        /href="https:\/\/dl\.google\.com\/[^\"]+_beta-ota-[^\"]+\.zip"/ {
            url=$0
            sub(/^.*href="/, "", url)
            sub(/".*$/, "", url)
            name=url
            sub(/^.*\//, "", name)
            device=name
            sub(/_beta-ota-.*$/, "", device)
            if (models[device] != "" && !seen[device]++)
                print device "\t" models[device] "\t" url
        }
    ' "$_input" > "$_output" 2>/dev/null || return 1
    [ -s "$_output" ]
}

safe_token() {
    [ -n "$1" ] && [ "${#1}" -le 128 ] || return 1
    case "$1" in *[!A-Za-z0-9._+-]*) return 1 ;; esac
}

safe_model() {
    [ -n "$1" ] && [ "${#1}" -le 128 ] || return 1
    case "$1" in
        *[!A-Za-z0-9._+\ -]*|*'  '*|' '*|*' ') return 1 ;;
    esac
}

valid_patch() {
    case "$1" in
        [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]) : ;;
        *) return 1 ;;
    esac
    _patch_year="${1%%-*}"
    _patch_rest="${1#*-}"
    _patch_month="${_patch_rest%%-*}"
    _patch_day="${_patch_rest#*-}"
    [ "$_patch_year" -ge 2008 ] && [ "$_patch_year" -le 2100 ] &&
        [ "$_patch_month" -ge 1 ] && [ "$_patch_month" -le 12 ] &&
        [ "$_patch_day" -ge 1 ] && [ "$_patch_day" -le 31 ]
}

parse_ota() {
    _metadata="$1"
    _expected_device="$2"
    _model="$3"
    _url="$4"
    _sdk="$5"
    _release="$6"
    _candidate="$7"

    _fingerprint="$(grep -am1 '^post-build=' "$_metadata" 2>/dev/null | cut -d= -f2-)"
    _patch="$(grep -am1 '^post-security-patch-level=' "$_metadata" 2>/dev/null | cut -d= -f2-)"
    [ -n "$_fingerprint" ] && [ -n "$_patch" ] || return 1
    case "$_fingerprint" in
        google/*/*:*/*/*:user/release-keys) : ;;
        *) return 1 ;;
    esac
    valid_patch "$_patch" || return 1

    _brand="${_fingerprint%%/*}"
    _rest="${_fingerprint#*/}"
    _product="${_rest%%/*}"
    _rest="${_rest#*/}"
    _device="${_rest%%:*}"
    _rest="${_rest#*:}"
    _fp_release="${_rest%%/*}"
    _rest="${_rest#*/}"
    _id="${_rest%%/*}"
    _rest="${_rest#*/}"
    _incremental="${_rest%%:*}"

    [ "$_brand" = google ] || return 1
    [ "$_device" = "$_expected_device" ] || return 1
    [ "$_fp_release" = "$_release" ] || return 1
    [ "$_product" = "${_device}_beta" ] || return 1
    safe_model "$_model" || return 1
    for _value in "$_device" "$_product" "$_id" "$_incremental"; do
        safe_token "$_value" || return 1
    done
    _platform="$(map_platform "$_device")" || return 1
    _soc="$(soc_model "$_platform")" || return 1
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\tgoogle\tGoogle\t%s\tGoogle\t%s\t\n' \
        "$_model" "$_device" "$_product" "$_device" "$_platform" \
        "$_sdk" "$_release" "$_id" "$_incremental" "$_patch" \
        "$_model" "$_soc" > "$_candidate" || return 1
    _lines="$(wc -l < "$_candidate" 2>/dev/null | tr -d ' ')"
    _bytes="$(wc -c < "$_candidate" 2>/dev/null | tr -d ' ')"
    [ "$_lines" = 1 ] && [ "$_bytes" -le 4096 ] || return 1
    SELECTED_URL="$_url"
    return 0
}

select_candidate() {
    _catalog="$1"
    _sdk="$2"
    _release="$3"
    _candidate="$4"
    _eligible="$TMP_DIR/eligible.tsv"
    _prior="$(prior_device "$_sdk")"

    while IFS="$(printf '\t')" read -r _device _model _url; do
        [ -n "$_device" ] && [ -n "$_model" ] && allowed_url "$_url" || continue
        map_platform "$_device" >/dev/null 2>&1 || continue
        printf '%s\t%s\t%s\n' "$_device" "$_model" "$_url"
    done < "$_catalog" > "$_eligible"
    _count="$(wc -l < "$_eligible" 2>/dev/null | tr -d ' ')"
    case "$_count" in ''|*[!0-9]*|0) return 1 ;; esac

    if [ -n "$_prior" ]; then
        _alternatives="$TMP_DIR/alternatives.tsv"
        awk -F '\t' -v prior="$_prior" '$1 != prior' "$_eligible" > "$_alternatives"
        [ -s "$_alternatives" ] || return 1
        mv -f "$_alternatives" "$_eligible" || return 1
        _count="$(wc -l < "$_eligible" 2>/dev/null | tr -d ' ')"
    fi
    _start="$(rand_below "$_count")" || return 1
    _tries=0
    while [ "$_tries" -lt "$_count" ] && [ "$_tries" -lt "$MAX_TRIES" ]; do
        _index=$(( (_start + _tries) % _count + 1 ))
        _tries=$((_tries + 1))
        _line="$(sed -n "${_index}p" "$_eligible")"
        _device="$(printf '%s\n' "$_line" | cut -f1)"
        _model="$(printf '%s\n' "$_line" | cut -f2)"
        _url="$(printf '%s\n' "$_line" | cut -f3)"
        _metadata="$TMP_DIR/ota-prefix"
        rm -f "$_metadata"
        _range_end=$((OTA_MAX_BYTES - 1))
        if ! fetch_url "$_url" "$_metadata" "$OTA_MAX_BYTES" "0-$_range_end"; then
            log "lewati $_device: metadata OTA tidak dapat diunduh"
            continue
        fi
        if parse_ota "$_metadata" "$_device" "$_model" "$_url" \
            "$_sdk" "$_release" "$_candidate"; then
            return 0
        fi
        log "lewati $_device: metadata OTA tidak lengkap atau tidak cocok"
    done
    return 1
}

cmd_refresh() {
    resolve_bin || {
        log "binary native tidak tersedia; cache lama dipertahankan"
        return 127
    }
    valid_bound "$CATALOG_MAX_BYTES" && valid_bound "$OTA_MAX_BYTES" || {
        log "batas unduhan tidak valid; cache lama dipertahankan"
        return 64
    }
    case "$MAX_TRIES" in ''|*[!0-9]*|0) return 64 ;; esac
    [ "$ADAPTER" = pixel-ota-v1 ] || {
        log "adapter tidak didukung; cache lama dipertahankan"
        return 64
    }
    _sdk="${SBX_REAL_SDK:-$(getprop ro.build.version.sdk 2>/dev/null)}"
    case "$_sdk" in ''|*[!0-9]*)
        log "SDK runtime tidak tersedia; cache lama dipertahankan"
        return 64 ;;
    esac
    _release="$(sdk_release "$_sdk")" || {
        log "SDK runtime $_sdk belum memiliki katalog OTA resmi yang didukung"
        return 64
    }
    _catalog_url="${AUTOPIF_CATALOG_URL:-https://developer.android.com/about/versions/$_release/download-ota}"
    allowed_url "$_catalog_url" || {
        log "URL katalog ditolak; cache lama dipertahankan"
        return 64
    }
    make_tmp || {
        log "direktori sementara privat tidak tersedia"
        return 1
    }
    trap cleanup_tmp EXIT
    trap 'on_signal 129' HUP
    trap 'on_signal 130' INT
    trap 'on_signal 143' TERM

    _raw="$TMP_DIR/catalog.html"
    _catalog="$TMP_DIR/catalog.tsv"
    _candidate="$TMP_DIR/candidate.tsv"
    _meta="$TMP_DIR/candidate.meta"
    fetch_url "$_catalog_url" "$_raw" "$CATALOG_MAX_BYTES"
    _rc=$?
    if [ "$_rc" -ne 0 ]; then
        log "unduhan katalog gagal (rc=$_rc); cache lama dipertahankan"
        return "$_rc"
    fi
    if ! extract_catalog "$_raw" "$_catalog"; then
        log "tabel OTA resmi tidak dapat diparse; cache lama dipertahankan"
        return 1
    fi
    if ! select_candidate "$_catalog" "$_sdk" "$_release" "$_candidate"; then
        log "tidak ada persona OTA exact-runtime lengkap; cache lama dipertahankan"
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
        printf 'url=%s\n' "$SELECTED_URL"
    } > "$_meta" || return 1
    "$BIN" persona-import "$_candidate" "$_meta"
    _rc=$?
    if [ "$_rc" -eq 0 ]; then
        log "persona Pixel OTA exact-SDK tervalidasi dan cache diperbarui"
        return 0
    fi
    log "validasi/import native gagal (rc=$_rc); cache lama dipertahankan"
    return "$_rc"
}

case "${1:-refresh}" in
    refresh|'') cmd_refresh ;;
    -h|--help|help)
        printf 'Usage: autopif.sh refresh\n'
        printf 'Action always attempts a bounded official Pixel OTA refresh.\n'
        exit 0 ;;
    *)
        log "subcommand tidak dikenal; gunakan: refresh"
        exit 64 ;;
esac
exit $?
