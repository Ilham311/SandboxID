#!/usr/bin/env bash
# Assertions evaluate variable names from test expressions.
# shellcheck disable=SC2034
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP_ROOT="${TMPDIR:-$ROOT/.claude-tmp}"
WORK=$(mktemp -d "$TMP_ROOT/sbx-autopif-test.XXXXXX") || exit 1
trap 'if [ "${SBX_KEEP_TEST_TMP:-0}" = 1 ]; then printf "kept: %s\\n" "$WORK"; else rm -rf "$WORK"; fi' EXIT

checks=0
failures=0
check() {
    checks=$((checks + 1))
    if ! eval "$1"; then
        failures=$((failures + 1))
        printf 'FAIL: %s\n' "$2"
    fi
}

mkdir -p "$WORK/mod/bin" "$WORK/tools"
cp "$ROOT/autopif.sh" "$WORK/mod/"
printf '%s\n' '#!/bin/sh' \
    'case "$1" in' \
    '  ro.build.version.sdk) printf "%s\n" "${SBX_REAL_SDK:-35}" ;;' \
    'esac' > "$WORK/tools/getprop"
printf '%s\n' '#!/bin/sh' \
    'printf "%s\n" "$*" > "$SBX_CURL_ARGS"' \
    '[ "${SBX_CURL_RC:-0}" -eq 0 ] || exit "$SBX_CURL_RC"' \
    'out=""; while [ "$#" -gt 0 ]; do' \
    '  [ "$1" = --output ] && { shift; out=$1; }' \
    '  shift' \
    'done' \
    '[ -n "$out" ] || exit 64' \
    'printf "%b" "${SBX_CURL_BODY:-remote}" > "$out"' > "$WORK/tools/curl"
printf '%s\n' '#!/bin/sh' \
    'printf "%s\n" "$*" >> "$SBX_NATIVE_CALLS"' \
    '[ "$1" = persona-import ] || exit 64' \
    'cp "$2" "$SBX_IMPORTED_CANDIDATE" || exit 1' \
    'cp "$3" "$SBX_IMPORTED_META" || exit 1' \
    'exit "${SBX_IMPORT_RC:-0}"' > "$WORK/mod/bin/sandboxid"
printf '%s\n' '#!/bin/sh' \
    'if [ "$#" -eq 0 ]; then printf "mock sha256sum\n"; exit 0; fi' \
    'exec /data/data/com.termux/files/usr/bin/sha256sum "$@"' > "$WORK/tools/sha256sum"
chmod 0755 "$WORK/tools/getprop" "$WORK/tools/curl" "$WORK/tools/sha256sum" "$WORK/mod/bin/sandboxid"

CANDIDATE='Pixel 8\tshiba\tshiba\tshiba\tzuma\t35\t15\tAP3A.240905.015\t12244875\t2024-09-05\tgoogle\tGoogle\tPixel 8\tGoogle\tTensor G3\t'
export SBX_CURL_ARGS="$WORK/curl.args" SBX_NATIVE_CALLS="$WORK/native.calls"
export SBX_IMPORTED_CANDIDATE="$WORK/imported.tsv" SBX_IMPORTED_META="$WORK/imported.meta"

: > "$WORK/cache"
: > "$WORK/cache.meta"
if PATH="$WORK/tools:$PATH" MODDIR="$WORK/mod" SBX_REAL_SDK=35 \
   PERSONA_CACHE="$WORK/cache" PERSONA_CACHE_META="$WORK/cache.meta" \
   sh "$WORK/mod/autopif.sh" refresh > "$WORK/disabled.log" 2>&1; then
    disabled_rc=0
else
    disabled_rc=$?
fi
check '[ "$disabled_rc" -eq 0 ]' 'disabled refresh is a no-op'
check '[ ! -e "$WORK/native.calls" ]' 'disabled refresh never imports'

touch "$WORK/mod/enable_remote_refresh"
printf '%s\n' keep-cache > "$WORK/cache"
printf '%s\n' keep-meta > "$WORK/cache.meta"
SBX_CURL_RC=22 PATH="$WORK/tools:$PATH" MODDIR="$WORK/mod" SBX_REAL_SDK=35 \
    PERSONA_CACHE="$WORK/cache" PERSONA_CACHE_META="$WORK/cache.meta" \
    sh "$WORK/mod/autopif.sh" refresh > "$WORK/fetch-fail.log" 2>&1
fetch_rc=$?
check '[ "$fetch_rc" -eq 22 ]' 'curl status is preserved'
check 'grep -q "unduhan gagal (rc=22)" "$WORK/fetch-fail.log"' 'fetch failure reports the original status'
check 'grep -q "^keep-cache$" "$WORK/cache" && grep -q "^keep-meta$" "$WORK/cache.meta"' 'fetch failure preserves last-known-good cache files'
check 'grep -q -- "--max-filesize 262144" "$WORK/curl.args"' 'fetch applies a streaming response-size bound'
check '! grep -q -- "--location" "$WORK/curl.args"' 'fetch refuses cross-host redirects instead of following them'

SBX_CURL_BODY='no complete persona marker\n' SBX_CURL_RC=0 \
    PATH="$WORK/tools:$PATH" MODDIR="$WORK/mod" SBX_REAL_SDK=35 \
    PERSONA_CACHE="$WORK/cache" PERSONA_CACHE_META="$WORK/cache.meta" \
    sh "$WORK/mod/autopif.sh" refresh > "$WORK/parse-fail.log" 2>&1
parse_rc=$?
check '[ "$parse_rc" -ne 0 ]' 'incomplete remote schema is rejected'
check 'grep -q "^keep-cache$" "$WORK/cache" && grep -q "^keep-meta$" "$WORK/cache.meta"' 'parse failure preserves cache files'

SBX_CURL_BODY="SBX_PERSONA_V1 ${CANDIDATE}\\n" SBX_CURL_RC=0 \
    PATH="$WORK/tools:$PATH" MODDIR="$WORK/mod" SBX_REAL_SDK=35 \
    PERSONA_CACHE="$WORK/cache" PERSONA_CACHE_META="$WORK/cache.meta" \
    sh "$WORK/mod/autopif.sh" refresh > "$WORK/success.log" 2>&1
success_rc=$?
check '[ "$success_rc" -eq 0 ]' 'complete explicit persona reaches native import'
check 'grep -q "^persona-import " "$WORK/native.calls"' 'native persona-import is authoritative'
check 'grep -q "^runtime_sdk=35$" "$WORK/imported.meta"' 'provenance records runtime SDK'
check 'grep -q "^adapter=complete-marker$" "$WORK/imported.meta"' 'provenance records constrained adapter'
check 'grep -q "^candidate_sha256=[0-9a-f][0-9a-f]*$" "$WORK/imported.meta"' 'provenance records candidate hash'

printf '\n%d checks, %d failures\n' "$checks" "$failures"
exit "$failures"
