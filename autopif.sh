#!/system/bin/sh
#
# autopif.sh — fetch ONE fresh, RANDOM Pixel persona from Google's live build
# data and hand it to `sandboxid freshen` as a one-shot override. Based on
# dannycreations' autopif.sh (canary fingerprint fetcher), adapted for on-device
# Android sh.
#
# What it does:
#   * Best-effort: if neither curl nor wget exists (stock Android usually ships
#     NEITHER), it exits 0 without touching anything — freshen then falls back to
#     the bundled personas.tsv pool, so behaviour is unchanged offline.
#   * When network + a downloader ARE present, it scrapes the latest Pixel CANARY
#     build for ONE RANDOMLY-CHOSEN Pixel model, derives the 10 persona columns,
#     and writes them to PERSONA_OVERRIDE (a single TSV line).
#   * `sandboxid freshen` (run right after, by action.sh) consumes that override:
#     it derives the identity straight from this persona — bypassing the
#     SDK-matched pool pick — and deletes the file. So EVERY run applies a NEW
#     random Pixel identity directly to identity.prop (no personas.tsv pool
#     accumulation, and never "locked" to one model).
#   * Devices whose SoC/platform we can't map are SKIPPED (never guessed) so a
#     spoofed RADIO / ro.board.platform can't go inconsistent.
#
# Why one model instead of the whole list: writing the override is a single
# random pick, so there is no reason to query every device. Fetching one (with a
# small retry) is dramatically faster than the old 15-model pool refresh.
#
# Exit status is ALWAYS 0 for a no-op / partial failure: refreshing the persona
# must never block `sandboxid freshen` in action.sh. On any failure we leave NO
# override behind, so freshen cleanly falls back to the bundled pool.

MODDIR="${MODDIR:-/data/adb/modules/sandboxid}"
OVERRIDE_FILE="${PERSONA_OVERRIDE:-$MODDIR/persona.override}"

# Current Pixel dev cycle. Canary always tracks the newest release, so these are
# constant per cycle; override via env when a new Android version ships.
CANARY_RELEASE="${CANARY_RELEASE:-16}"
CANARY_SDK="${CANARY_SDK:-36}"

# How many random models to try before giving up (each try = one extra HTTP
# request). Small: the first pick almost always resolves; this only covers a
# model whose canary block is momentarily missing.
MAX_TRY="${AUTOPIF_MAX_TRY:-4}"

log() { echo "[autopif] $*"; }

# Clean-slate: drop any stale override up front so that if THIS run fails we fall
# back to the bundled pool rather than silently reusing a previous fetch.
rm -f "$OVERRIDE_FILE" 2>/dev/null

# --- 0. Downloader guard -----------------------------------------------------
DL=""
if command -v curl >/dev/null 2>&1; then
  DL="curl"
elif command -v wget >/dev/null 2>&1; then
  DL="wget"
else
  log "no curl/wget on device — freshen will use bundled personas.tsv (offline no-op)"
  exit 0
fi

download() {
  # download <url> <outfile> [referer]
  _url="$1"; _out="$2"; _ref="$3"
  if [ "$DL" = "curl" ]; then
    if [ -n "$_ref" ]; then
      curl -sL --connect-timeout 10 --max-time 40 -H "Referer: $_ref" "$_url" -o "$_out" 2>/dev/null
    else
      curl -sL --connect-timeout 10 --max-time 40 "$_url" -o "$_out" 2>/dev/null
    fi
  else
    if [ -n "$_ref" ]; then
      wget -T 10 -qO "$_out" --header "Referer: $_ref" "$_url" 2>/dev/null
    else
      wget -T 10 -qO "$_out" "$_url" 2>/dev/null
    fi
  fi
}

# Random integer in [0, $1). Reads 3 bytes from /dev/urandom via toybox `od`
# (same -tx1 pattern helpers.sh already relies on); 3 bytes stay positive even in
# a 32-bit shell. Falls back to the PID if urandom is unreadable so a pick is
# always made.
rand_below() {
  _n="$1"
  [ "$_n" -gt 0 ] 2>/dev/null || { echo 0; return; }
  _h=$(od -An -N3 -tx1 /dev/urandom 2>/dev/null | tr -d ' \n')
  if [ -n "$_h" ]; then _r=$(( 0x$_h )); else _r=$$; fi
  echo $(( _r % _n ))
}

# --- Temp workspace ----------------------------------------------------------
TMP_DIR=$(mktemp -d 2>/dev/null)
if [ -z "$TMP_DIR" ] || [ ! -d "$TMP_DIR" ]; then
  TMP_DIR="${TMPDIR:-/data/local/tmp}/autopif.$$"
  mkdir -p "$TMP_DIR" 2>/dev/null || { log "no writable temp dir — aborting refresh"; exit 0; }
fi
cleanup() { rm -rf "$TMP_DIR" 2>/dev/null; }
trap cleanup EXIT

cd "$TMP_DIR" 2>/dev/null || { log "cannot enter temp dir — aborting"; exit 0; }

# --- device codename -> SoC/platform (must match modem_prefix() in sandboxid.cpp)
# Unknown codenames return "" and are SKIPPED, never guessed.
map_platform() {
  case "$1" in
    oriole|bluejay|raven)                 echo "gs101" ;;
    panther|cheetah|lynx)                 echo "gs201" ;;
    shiba|husky|akita)                    echo "zuma" ;;
    tokay|caiman|komodo|tegu|comet)       echo "zumapro" ;;
    frankel|blazer|mustang|rango)         echo "laguna" ;;
    *)                                    echo "" ;;
  esac
}

# --- 1. Latest Pixel version page --------------------------------------------
download "https://developer.android.com/about/versions" "versions.html"
# Prefix each candidate URL with its trailing version number so sort -n orders
# by numeric value (plain `sort -ru` is lexicographic and would rank "9" above
# "16"), then strip the key back off before taking the top result.
LATEST_URL=$(grep -o 'https://developer.android.com/about/versions/.*[0-9]"' "versions.html" 2>/dev/null | sed 's;.*/\([0-9][0-9]*\)"$;\1 &;' | sort -rn | cut -d' ' -f2- | cut -d\" -f1 | head -n1)
[ -z "$LATEST_URL" ] && { log "could not resolve latest versions URL — keeping bundled pool"; exit 0; }
download "$LATEST_URL" "latest.html"

# --- 2. Factory-image (device info) page -------------------------------------
FI_PATH=$(grep -o 'href=".*download.*"' "latest.html" 2>/dev/null | grep 'qpr' | cut -d\" -f2 | head -n1)
[ -z "$FI_PATH" ] && { log "no factory-image link found — keeping bundled pool"; exit 0; }
download "https://developer.android.com$FI_PATH" "fi.html"

MODEL_LIST=$(grep -A1 'tr id=' "fi.html" 2>/dev/null | grep 'td' | sed 's;.*<td>\(.*\)</td>.*;\1;')
PRODUCT_LIST=$(grep 'tr id=' "fi.html" 2>/dev/null | sed 's;.*<tr id="\(.*\)">.*;\1;')

count_model=$(printf '%s\n' "$MODEL_LIST"   | grep -c .)
count_prod=$(printf '%s\n'  "$PRODUCT_LIST" | grep -c .)
if [ "$count_model" -eq 0 ] || [ "$count_model" -ne "$count_prod" ]; then
  log "device table parse failed (models=$count_model products=$count_prod) — keeping bundled pool"
  exit 0
fi

# --- 3. Which rows have a SoC we can map? ------------------------------------
# Collect the 1-based indices of models with a known platform; SKIP the rest so
# a spoofed RADIO / ro.board.platform can never go inconsistent.
known=""
i=1
while [ "$i" -le "$count_model" ]; do
  device=$(printf '%s\n' "$PRODUCT_LIST" | sed -n "${i}p")
  [ -n "$device" ] && [ -n "$(map_platform "$device")" ] && known="$known $i"
  i=$((i + 1))
done

set -- $known            # positional params $1..$# = the known-platform indices
kn=$#
if [ "$kn" -eq 0 ]; then
  log "no known-SoC models in Google's list — keeping bundled pool"
  exit 0
fi

# --- 4. Flash-tool API key + security bulletin (fetched once) ----------------
download "https://flash.android.com" "flash.html"
FLASH_KEY=$(grep -o '<body data-client-config=.*' "flash.html" 2>/dev/null | cut -d\; -f2 | cut -d\& -f1)
[ -z "$FLASH_KEY" ] && { log "could not obtain flash API key — keeping bundled pool"; exit 0; }

download "https://source.android.com/docs/security/bulletin/pixel" "secbull.html"

# --- 5. Resolve ONE random model's canary build into a persona line ----------
# Given a 1-based row index, fetch its canary build and print the 10-column
# persona TSV line on success (return 0), or return 1 to let the caller try
# another random model.
resolve_persona() {
  _idx="$1"
  _model=$(printf '%s\n'  "$MODEL_LIST"   | sed -n "${_idx}p")
  _device=$(printf '%s\n' "$PRODUCT_LIST" | sed -n "${_idx}p")
  [ -z "$_model" ] && return 1
  [ -z "$_device" ] && return 1
  _platform=$(map_platform "$_device")
  [ -z "$_platform" ] && return 1

  # flashstation lists canary/beta builds under the "<codename>_beta" product.
  _station_url="https://content-flashstation-pa.googleapis.com/v1/builds?product=${_device}_beta&key=$FLASH_KEY"
  download "$_station_url" "station.json" "https://flash.android.com"
  [ -s "station.json" ] || { log "skip $_model: no build data"; return 1; }

  # newest canary build block
  if command -v tac >/dev/null 2>&1; then
    tac "station.json" | grep -m1 -A13 '"canary": true' > "canary.json" 2>/dev/null
  else
    grep -A20 '"canary": true' "station.json" 2>/dev/null | head -n 20 > "canary.json"
  fi
  [ -s "canary.json" ] || { log "skip $_model: no canary build"; return 1; }

  _bid=$(grep 'releaseCandidateName' "canary.json" | cut -d\" -f4 | head -n1)
  _incr=$(grep 'buildId' "canary.json" | cut -d\" -f4 | head -n1)
  [ -z "$_bid" ] && { log "skip $_model: no build id"; return 1; }
  [ -z "$_incr" ] && { log "skip $_model: no incremental"; return 1; }

  # security patch: match canary id (YYYY-MM) against the bulletin, else fall
  # back to that month + "-05" like the reference script.
  _canary_id=$(grep '"id"' "canary.json" | sed -e 's;.*canary-\(.*\)";\1;' -e 's;^\(.\{4\}\);\1-;' | head -n1)
  _spatch=""
  if [ -n "$_canary_id" ] && [ -s "secbull.html" ]; then
    _spatch=$(grep "<td>$_canary_id" "secbull.html" 2>/dev/null | sed 's;.*<td>\(.*\)</td>;\1;' | head -n1)
  fi
  [ -z "$_spatch" ] && [ -n "$_canary_id" ] && _spatch="${_canary_id}-05"
  [ -z "$_spatch" ] && { log "skip $_model: no security patch"; return 1; }

  # persona columns: product/board == codename (matches curated pool style).
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$_model" "$_device" "$_device" "$_device" "$_platform" \
    "$CANARY_SDK" "$CANARY_RELEASE" "$_bid" "$_incr" "$_spatch"
  return 0
}

# Random starting offset, then walk the known list (wrapping) so retries hit
# DIFFERENT models. Cap the walk at MAX_TRY so a bad run still ends quickly.
start=$(rand_below "$kn")
persona_line=""
try=0
while [ "$try" -lt "$kn" ] && [ "$try" -lt "$MAX_TRY" ]; do
  off=$(( (start + try) % kn ))
  try=$((try + 1))
  eval "row=\${$((off + 1))}"        # off-th known index (positional params)
  persona_line=$(resolve_persona "$row")
  [ -n "$persona_line" ] && break
done

if [ -z "$persona_line" ]; then
  log "no canary persona resolved in $try tr(y|ies) — keeping bundled pool"
  exit 0
fi

# --- 6. Write the one-shot override (atomic) ---------------------------------
if printf '%s\n' "$persona_line" > "$OVERRIDE_FILE.tmp.$$" 2>/dev/null \
   && mv -f "$OVERRIDE_FILE.tmp.$$" "$OVERRIDE_FILE" 2>/dev/null; then
  chmod 0644 "$OVERRIDE_FILE" 2>/dev/null
  # model \t device \t ... — log just the human bits.
  _m=$(printf '%s' "$persona_line" | cut -f1)
  _d=$(printf '%s' "$persona_line" | cut -f2)
  _b=$(printf '%s' "$persona_line" | cut -f8)
  log "fetched random canary persona: $_m ($_d) build $_b — freshen will apply it directly"
else
  rm -f "$OVERRIDE_FILE.tmp.$$" 2>/dev/null
  log "could not write persona override — keeping bundled pool"
fi

exit 0
