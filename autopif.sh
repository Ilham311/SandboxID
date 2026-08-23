#!/system/bin/sh
#
# autopif.sh — refresh SandboxID's persona pool from Google's live Pixel build
# data. Based on dannycreations' autopif.sh (canary fingerprint fetcher),
# adapted for on-device Android sh and to feed SandboxID's persona pool
# (personas.tsv) instead of writing a PlayIntegrityFix pif.json.
#
# What it does:
#   * Best-effort: if neither curl nor wget exists (stock Android usually ships
#     NEITHER), it exits 0 without touching anything — the bundled personas.tsv
#     (curated stable Pixels) stays in force, so behaviour is unchanged offline.
#   * When network + a downloader ARE present, it scrapes the latest Pixel
#     CANARY build for each Pixel model, derives the 10 persona columns, and
#     UPSERTS them into personas.tsv (replacing any row for the same codename).
#   * Devices whose SoC/platform we can't map are SKIPPED (never guessed) so a
#     spoofed RADIO / ro.board.platform can't go inconsistent.
#
# "Both channels" = the bundled personas.tsv provides STABLE release personas;
# this fetcher layers fresh CANARY personas on top. gen_identity() then picks
# (SDK-matched, random) across the combined pool.
#
# Exit status is ALWAYS 0 for a no-op / partial failure: refreshing the pool
# must never block `sandboxid freshen` in action.sh.

MODDIR="${MODDIR:-/data/adb/modules/sandboxid}"
PERSONAS_FILE="${PERSONAS_FILE:-$MODDIR/personas.tsv}"

# Current Pixel dev cycle. Canary always tracks the newest release, so these are
# constant per cycle; override via env when a new Android version ships.
CANARY_RELEASE="${CANARY_RELEASE:-16}"
CANARY_SDK="${CANARY_SDK:-36}"

# Cap how many models we query so a huge list can't stall boot/Action.
MAX_MODELS="${AUTOPIF_MAX_MODELS:-15}"

log() { echo "[autopif] $*"; }

# --- 0. Downloader guard -----------------------------------------------------
DL=""
if command -v curl >/dev/null 2>&1; then
  DL="curl"
elif command -v wget >/dev/null 2>&1; then
  DL="wget"
else
  log "no curl/wget on device — keeping bundled personas.tsv (offline no-op)"
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

# --- 3. Flash-tool API key + security bulletin (fetched once) ----------------
download "https://flash.android.com" "flash.html"
FLASH_KEY=$(grep -o '<body data-client-config=.*' "flash.html" 2>/dev/null | cut -d\; -f2 | cut -d\& -f1)
[ -z "$FLASH_KEY" ] && { log "could not obtain flash API key — keeping bundled pool"; exit 0; }

download "https://source.android.com/docs/security/bulletin/pixel" "secbull.html"

# --- 4. Per-device: fetch canary build, derive persona row -------------------
: > "newrows.tsv"
added=0
i=1
while [ "$i" -le "$count_model" ] && [ "$i" -le "$MAX_MODELS" ]; do
  model=$(printf '%s\n'   "$MODEL_LIST"   | sed -n "${i}p")
  device=$(printf '%s\n'  "$PRODUCT_LIST" | sed -n "${i}p")
  i=$((i + 1))

  [ -z "$model" ] && continue
  [ -z "$device" ] && continue

  platform=$(map_platform "$device")
  if [ -z "$platform" ]; then
    log "skip $model ($device): unknown SoC — leaving to bundled stable pool"
    continue
  fi

  # flashstation lists canary/beta builds under the "<codename>_beta" product.
  station_url="https://content-flashstation-pa.googleapis.com/v1/builds?product=${device}_beta&key=$FLASH_KEY"
  download "$station_url" "station.json" "https://flash.android.com"
  [ -s "station.json" ] || { log "skip $model: no build data"; continue; }

  # newest canary build block
  if command -v tac >/dev/null 2>&1; then
    tac "station.json" | grep -m1 -A13 '"canary": true' > "canary.json" 2>/dev/null
  else
    grep -A20 '"canary": true' "station.json" 2>/dev/null | head -n 20 > "canary.json"
  fi
  [ -s "canary.json" ] || { log "skip $model: no canary build"; continue; }

  bid=$(grep 'releaseCandidateName' "canary.json" | cut -d\" -f4 | head -n1)
  incr=$(grep 'buildId' "canary.json" | cut -d\" -f4 | head -n1)
  [ -z "$bid" ] && { log "skip $model: no build id"; continue; }
  [ -z "$incr" ] && { log "skip $model: no incremental"; continue; }

  # security patch: match canary id (YYYY-MM) against the bulletin, else fall
  # back to that month + "-05" like the reference script.
  canary_id=$(grep '"id"' "canary.json" | sed -e 's;.*canary-\(.*\)";\1;' -e 's;^\(.\{4\}\);\1-;' | head -n1)
  spatch=""
  if [ -n "$canary_id" ] && [ -s "secbull.html" ]; then
    spatch=$(grep "<td>$canary_id" "secbull.html" 2>/dev/null | sed 's;.*<td>\(.*\)</td>;\1;' | head -n1)
  fi
  [ -z "$spatch" ] && [ -n "$canary_id" ] && spatch="${canary_id}-05"
  [ -z "$spatch" ] && { log "skip $model: no security patch"; continue; }

  # persona columns: product/board == codename (matches curated pool style).
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$model" "$device" "$device" "$device" "$platform" \
    "$CANARY_SDK" "$CANARY_RELEASE" "$bid" "$incr" "$spatch" >> "newrows.tsv"
  added=$((added + 1))
  log "canary $model ($device/$platform): $bid / $incr / $spatch"
done

if [ "$added" -eq 0 ]; then
  log "no canary personas resolved — keeping bundled pool unchanged"
  exit 0
fi

# --- 5. Upsert into personas.tsv (replace rows with same codename) -----------
[ -f "$PERSONAS_FILE" ] || : > "$PERSONAS_FILE"
merged="$TMP_DIR/personas.merged"

awk -F'\t' -v newf="$TMP_DIR/newrows.tsv" '
  BEGIN {
    while ((getline l < newf) > 0) {
      if (l ~ /^#/ || l == "") continue
      n = split(l, a, "\t"); if (n >= 2) skip[a[2]] = 1
    }
    close(newf)
  }
  /^#/ || NF == 0 { print; next }     # preserve comments / blank lines
  !($2 in skip)  { print }            # keep existing rows we are NOT replacing
' "$PERSONAS_FILE" > "$merged" 2>/dev/null || { log "merge failed — keeping bundled pool"; exit 0; }

cat "newrows.tsv" >> "$merged"

# atomic replace
if cp -f "$merged" "$PERSONAS_FILE.tmp.$$" 2>/dev/null && mv -f "$PERSONAS_FILE.tmp.$$" "$PERSONAS_FILE" 2>/dev/null; then
  chmod 0644 "$PERSONAS_FILE" 2>/dev/null
  total=$(grep -cv '^#' "$PERSONAS_FILE" 2>/dev/null)
  log "upserted $added canary persona(s); pool now has $total entries"
else
  rm -f "$PERSONAS_FILE.tmp.$$" 2>/dev/null
  log "could not write personas.tsv — keeping bundled pool"
fi

exit 0
