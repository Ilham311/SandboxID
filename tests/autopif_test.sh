#!/usr/bin/env bash
# Assertions evaluate variable names from test expressions.
# shellcheck disable=SC2034
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP_ROOT="${TMPDIR:-$ROOT/.claude-tmp}"
mkdir -p "$TMP_ROOT"
WORK=$(mktemp -d "$TMP_ROOT/sbx-autopif-test.XXXXXX") || exit 1
trap 'if [ "${SBX_KEEP_TEST_TMP:-0}" = 1 ]; then printf "kept: %s\n" "$WORK"; else rm -rf "$WORK"; fi' EXIT

checks=0
failures=0
check() {
    checks=$((checks + 1))
    if ! eval "$1"; then
        failures=$((failures + 1))
        printf 'FAIL: %s\n' "$2"
    fi
}

mkdir -p "$WORK/mod/bin" "$WORK/tools" "$WORK/responses"
cp "$ROOT/autopif.sh" "$WORK/mod/"

cat > "$WORK/tools/getprop" <<'SH'
#!/bin/sh
case "$1" in
  ro.build.version.sdk) printf '%s\n' "${SBX_REAL_SDK:-35}" ;;
esac
SH

cat > "$WORK/tools/curl" <<'SH'
#!/bin/sh
printf '%s\n' "$*" >> "$SBX_CURL_ARGS"
out=""; range=""; url=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --output) shift; out=$1 ;;
    --range) shift; range=$1 ;;
    https://*) url=$1 ;;
  esac
  shift
done
[ -n "$out" ] && [ -n "$url" ] || exit 64
case "$url" in
  *developer.android.com/about/versions/14/download-ota*) src="$SBX_CATALOG_BODY"; rc="${SBX_CATALOG_RC:-22}" ;;
  *developer.android.com*) src="$SBX_CATALOG_BODY"; rc="${SBX_CATALOG_RC:-0}" ;;
  *shiba_beta*) src="${SBX_SHIBA_BODY:-$SBX_OTA_BODY}"; rc="${SBX_SHIBA_RC:-${SBX_OTA_RC:-0}}" ;;
  *husky_beta*) src="${SBX_HUSKY_BODY:-$SBX_OTA_BODY}"; rc="${SBX_HUSKY_RC:-${SBX_OTA_RC:-0}}" ;;
  *unknownpixel_beta*) src="${SBX_UNKNOWN_BODY:-$SBX_OTA_BODY}"; rc="${SBX_UNKNOWN_RC:-${SBX_OTA_RC:-0}}" ;;
  *) exit 22 ;;
esac
[ "$rc" -eq 0 ] || exit "$rc"
cp "$src" "$out" || exit 1
if [ -n "$range" ]; then
  end=${range#*-}
  size=$(wc -c < "$out")
  [ "$size" -le $((end + 1)) ] || dd if="$out" of="$out.trim" bs=1 count=$((end + 1)) 2>/dev/null
  [ ! -f "$out.trim" ] || mv "$out.trim" "$out"
fi
SH

cat > "$WORK/mod/bin/sandboxid" <<'SH'
#!/bin/sh
printf '%s\n' "$*" >> "$SBX_NATIVE_CALLS"
[ "$1" = persona-import ] || exit 64
cp "$2" "$SBX_IMPORTED_CANDIDATE" || exit 1
cp "$3" "$SBX_IMPORTED_META" || exit 1
exit "${SBX_IMPORT_RC:-0}"
SH

cat > "$WORK/tools/sha256sum" <<'SH'
#!/bin/sh
if [ "$#" -eq 0 ]; then printf 'mock sha256sum\n'; exit 0; fi
exec /data/data/com.termux/files/usr/bin/sha256sum "$@"
SH
chmod 0755 "$WORK/tools/"* "$WORK/mod/bin/sandboxid"

make_catalog() {
    cat > "$1" <<'HTML'
<tr id="shiba">
<td>Pixel 8</td>
</tr>
<tr id="husky">
<td>Pixel 8 Pro</td>
</tr>
<div id="shiba_ota_zip">
<a href="https://dl.google.com/developers/android/vic/images/ota/shiba_beta-ota-bp11.241210.004-a1bcf4f0.zip">Download</a>
</div>
<div id="husky_ota_zip">
<a href="https://dl.google.com/developers/android/vic/images/ota/husky_beta-ota-bp11.241210.004-feedbeef.zip">Download</a>
</div>
HTML
}

make_ota() {
    device=$1
    product=$2
    model_file=$3
    release=${4:-15}
    patch=${5:-2025-01-05}
    id=${6:-BP11.241210.004}
    {
      printf 'PK\003\004mock\n'
      printf 'post-build=google/%s/%s:%s/%s/12926906:user/release-keys\n' "$product" "$device" "$release" "$id"
      printf 'post-security-patch-level=%s\n' "$patch"
    } > "$model_file"
}

CATALOG="$WORK/responses/catalog.html"
SHIBA="$WORK/responses/shiba.bin"
HUSKY="$WORK/responses/husky.bin"
make_catalog "$CATALOG"
make_ota shiba shiba_beta "$SHIBA"
make_ota husky husky_beta "$HUSKY"

export SBX_CATALOG_BODY="$CATALOG" SBX_SHIBA_BODY="$SHIBA" SBX_HUSKY_BODY="$HUSKY"
export SBX_OTA_BODY="$SHIBA" SBX_CURL_ARGS="$WORK/curl.args"
export SBX_NATIVE_CALLS="$WORK/native.calls" SBX_IMPORTED_CANDIDATE="$WORK/imported.tsv"
export SBX_IMPORTED_META="$WORK/imported.meta"

run_refresh() {
    : > "$WORK/curl.args"
    rm -f "$WORK/native.calls" "$WORK/imported.tsv" "$WORK/imported.meta"
    PATH="$WORK/tools:$PATH" MODDIR="$WORK/mod" SBX_REAL_SDK="${1:-35}" \
      PERSONA_CACHE="$WORK/cache" PERSONA_CACHE_META="$WORK/cache.meta" \
      AUTOPIF_RANDOM="${AUTOPIF_RANDOM:-0}" SBX_IMPORT_RC="${SBX_IMPORT_RC:-0}" \
      sh "$WORK/mod/autopif.sh" refresh > "$WORK/run.log" 2>&1
}

printf 'keep-cache\n' > "$WORK/cache"
printf 'keep-meta\n' > "$WORK/cache.meta"
AUTOPIF_RANDOM=0 run_refresh 35
success_rc=$?
check '[ "$success_rc" -eq 0 ]' 'official exact-release OTA metadata reaches native import'
check 'grep -q $'"'"'^Pixel 8\tshiba\tshiba_beta\tshiba\tzuma\t35\t15\tBP11.241210.004\t12926906\t2025-01-05\tgoogle\tGoogle\tPixel 8\tGoogle\tGS301\t$'"'"' "$WORK/imported.tsv"' 'candidate preserves beta product and emits a complete 16-column Pixel row'
check 'grep -q "^adapter=pixel-ota-v1$" "$WORK/imported.meta" && grep -q "^url=https://dl.google.com/.*/shiba_beta-ota-" "$WORK/imported.meta"' 'provenance binds the Pixel OTA adapter and selected official URL'
check 'grep -q "^persona-import " "$WORK/native.calls"' 'native persona-import remains authoritative'
check 'grep -q -- "--max-filesize 524288" "$WORK/curl.args" && grep -q -- "--range 0-524287" "$WORK/curl.args"' 'catalog and OTA prefix have separate 512 KiB streaming bounds'
check '! grep -q -- "--location" "$WORK/curl.args"' 'fetch refuses implicit redirects'

AUTOPIF_CATALOG_URL='https://dl.google.com/unrelated.zip' run_refresh 35
allowlist_rc=$?
unset AUTOPIF_CATALOG_URL
check '[ "$allowlist_rc" -eq 64 ] && [ ! -e "$WORK/native.calls" ]' 'URL allowlist rejects unrelated paths even on an approved host'

AUTOPIF_CATALOG_URL='https://dl.google.com/developers/android/vic/images/ota/shiba.zip' run_refresh 35
non_beta_rc=$?
unset AUTOPIF_CATALOG_URL
check '[ "$non_beta_rc" -eq 64 ] && [ ! -e "$WORK/native.calls" ]' 'OTA allowlist rejects a safe non-beta ZIP name in the official directory'

printf 'Pixel 8\tshiba\tshiba_beta\tshiba\tzuma\t35\t15\tOLD\t1\t2024-01-05\tgoogle\tGoogle\tPixel 8\tGoogle\tGS301\t\n' > "$WORK/cache"
cache_hash=$(sha256sum "$WORK/cache" | cut -d' ' -f1)
printf 'version=1\nschema=persona-v1\nparser=autopif-v1\nretrieved_utc=1720000000\nruntime_sdk=35\ncandidate_sha256=%s\nadapter=pixel-ota-v1\nurl=https://dl.google.com/developers/android/vic/images/ota/shiba_beta-ota-old.zip\n' "$cache_hash" > "$WORK/cache.meta"
AUTOPIF_RANDOM=0 run_refresh 35
rotate_rc=$?
check '[ "$rotate_rc" -eq 0 ] && grep -q $'"'"'^Pixel 8 Pro\thusky\thusky_beta'"'"' "$WORK/imported.tsv"' 'validated prior device is excluded when a complete alternative exists'
check '! grep -q "shiba_beta-ota" "$WORK/imported.meta"' 'successful non-repeat provenance names the alternative OTA'

cat > "$WORK/responses/single.html" <<'HTML'
<tr id="shiba">
<td>Pixel 8</td>
</tr>
<div id="shiba_ota_zip">
<a href="https://dl.google.com/developers/android/vic/images/ota/shiba_beta-ota-bp11.241210.004-a1bcf4f0.zip">Download</a>
</div>
HTML
SBX_CATALOG_BODY="$WORK/responses/single.html" run_refresh 35
single_rc=$?
check '[ "$single_rc" -ne 0 ] && [ ! -e "$WORK/native.calls" ]' 'refresh refuses to repeat the validated prior device when no complete alternative exists'
check 'grep -q $'"'"'^Pixel 8\tshiba\tshiba_beta'"'"' "$WORK/cache" && grep -q "^candidate_sha256=$cache_hash$" "$WORK/cache.meta"' 'no-alternative failure preserves the validated prior cache pair'

printf 'keep-cache\n' > "$WORK/cache"
printf 'keep-meta\n' > "$WORK/cache.meta"
SBX_CATALOG_RC=22 run_refresh 35
fetch_rc=$?
unset SBX_CATALOG_RC
check '[ "$fetch_rc" -eq 22 ]' 'catalog curl status is preserved'
check 'grep -q "^keep-cache$" "$WORK/cache" && grep -q "^keep-meta$" "$WORK/cache.meta"' 'catalog failure preserves last-known-good cache files'

printf '<html>no OTA table</html>\n' > "$WORK/responses/empty.html"
SBX_CATALOG_BODY="$WORK/responses/empty.html" run_refresh 35
parse_rc=$?
check '[ "$parse_rc" -ne 0 ] && [ ! -e "$WORK/native.calls" ]' 'missing OTA table is rejected before native import'
check 'grep -q "^keep-cache$" "$WORK/cache" && grep -q "^keep-meta$" "$WORK/cache.meta"' 'catalog parse failure preserves cache files'

make_ota shiba shiba_beta "$WORK/responses/wrong-release.bin" 16
SBX_SHIBA_BODY="$WORK/responses/wrong-release.bin" SBX_HUSKY_RC=22 AUTOPIF_RANDOM=0 run_refresh 35
release_rc=$?
unset SBX_HUSKY_RC
check '[ "$release_rc" -ne 0 ] && [ ! -e "$WORK/native.calls" ]' 'runtime-release mismatch is rejected'

cat > "$WORK/responses/unknown.html" <<'HTML'
<tr id="unknownpixel">
<td>Unknown Pixel</td>
</tr>
<div id="unknownpixel_ota_zip">
<a href="https://dl.google.com/developers/android/vic/images/ota/unknownpixel_beta-ota-build.zip">Download</a>
</div>
HTML
SBX_CATALOG_BODY="$WORK/responses/unknown.html" run_refresh 35
unknown_rc=$?
check '[ "$unknown_rc" -ne 0 ] && [ ! -e "$WORK/native.calls" ]' 'unmapped Pixel device is rejected rather than guessed'

printf 'post-build=google/shiba_beta/shiba:15/BP11.241210.004/12926906:user/release-keys\n' > "$WORK/responses/missing-patch.bin"
SBX_SHIBA_BODY="$WORK/responses/missing-patch.bin" SBX_HUSKY_RC=22 AUTOPIF_RANDOM=0 run_refresh 35
metadata_rc=$?
unset SBX_HUSKY_RC
check '[ "$metadata_rc" -ne 0 ] && [ ! -e "$WORK/native.calls" ]' 'missing authoritative security patch is rejected'

make_ota shiba shiba_beta "$WORK/responses/invalid-patch.bin" 15 2025-13-05
SBX_SHIBA_BODY="$WORK/responses/invalid-patch.bin" SBX_HUSKY_RC=22 AUTOPIF_RANDOM=0 run_refresh 35
patch_rc=$?
unset SBX_HUSKY_RC
check '[ "$patch_rc" -ne 0 ] && [ ! -e "$WORK/native.calls" ]' 'out-of-range authoritative security patch is rejected before import'

SBX_IMPORT_RC=64 SBX_CATALOG_BODY="$CATALOG" SBX_SHIBA_BODY="$SHIBA" SBX_HUSKY_BODY="$HUSKY" AUTOPIF_RANDOM=0 run_refresh 35
import_rc=$?
unset SBX_IMPORT_RC
check '[ "$import_rc" -eq 64 ]' 'native import failure status is preserved'
check 'grep -q "^keep-cache$" "$WORK/cache" && grep -q "^keep-meta$" "$WORK/cache.meta"' 'native import failure preserves cache files'

run_refresh 34
old_release_rc=$?
check '[ "$old_release_rc" -eq 64 ] && grep -q "belum memiliki katalog OTA resmi yang didukung" "$WORK/run.log"' 'runtime releases without a stable exact catalog fail closed before networking'

sh -n "$ROOT/autopif.sh" || failures=$((failures + 1))
printf '\n%d checks, %d failures\n' "$checks" "$failures"
exit "$failures"
