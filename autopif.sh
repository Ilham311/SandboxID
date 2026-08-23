#!/system/bin/sh

# autopif fetches ONE fresh Google Pixel *canary* persona (latest SDK/release)
# straight from Google's flash-station API and writes it to persona.override,
# which `freshen` applies directly. This online path is Pixel-only by nature —
# only Google publishes an open build API.
#
# Multi-brand / non-Google devices are supported through the OFFLINE pool in
# personas.tsv instead (Samsung/Xiaomi/… rows with the optional brand columns);
# `freshen` picks from there, matched to the device's real SDK, whenever no
# override is present.
#
# Because the fetched canary is always the LATEST Android, `freshen` will REFUSE
# to apply this override on a device running an older Android (it never presents
# a higher SDK than the device runs) and falls back to the SDK-matched pool pick.
# So on Android 12-15 devices autopif is effectively a no-op and the pool drives
# the identity; on Android 16 devices it applies the fresh canary.

MODDIR="${MODDIR:-/data/adb/modules/sandboxid}"
OVERRIDE_FILE="${PERSONA_OVERRIDE:-$MODDIR/persona.override}"

CANARY_RELEASE="${CANARY_RELEASE:-16}"
CANARY_SDK="${CANARY_SDK:-36}"

MAX_TRY="${AUTOPIF_MAX_TRY:-4}"

log() { echo "[autopif] $*"; }

rm -f "$OVERRIDE_FILE" 2>/dev/null

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

rand_below() {
  _n="$1"
  [ "$_n" -gt 0 ] 2>/dev/null || { echo 0; return; }
  _h=$(od -An -N3 -tx1 /dev/urandom 2>/dev/null | tr -d ' \n')
  if [ -n "$_h" ]; then _r=$(( 0x$_h )); else _r=$$; fi
  echo $(( _r % _n ))
}

TMP_DIR=$(mktemp -d 2>/dev/null)
if [ -z "$TMP_DIR" ] || [ ! -d "$TMP_DIR" ]; then
  TMP_DIR="${TMPDIR:-/data/local/tmp}/autopif.$$"
  mkdir -p "$TMP_DIR" 2>/dev/null || { log "no writable temp dir — aborting refresh"; exit 0; }
fi
cleanup() { rm -rf "$TMP_DIR" 2>/dev/null; }
trap cleanup EXIT

cd "$TMP_DIR" 2>/dev/null || { log "cannot enter temp dir — aborting"; exit 0; }

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

download "https://developer.android.com/about/versions" "versions.html"
LATEST_URL=$(grep -o 'https://developer.android.com/about/versions/.*[0-9]"' "versions.html" 2>/dev/null | sed 's;.*/\([0-9][0-9]*\)"$;\1 &;' | sort -rn | cut -d' ' -f2- | cut -d\" -f1 | head -n1)
[ -z "$LATEST_URL" ] && { log "could not resolve latest versions URL — keeping bundled pool"; exit 0; }
download "$LATEST_URL" "latest.html"

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

known=""
i=1
while [ "$i" -le "$count_model" ]; do
  device=$(printf '%s\n' "$PRODUCT_LIST" | sed -n "${i}p")
  [ -n "$device" ] && [ -n "$(map_platform "$device")" ] && known="$known $i"
  i=$((i + 1))
done

set -- $known
kn=$#
if [ "$kn" -eq 0 ]; then
  log "no known-SoC models in Google's list — keeping bundled pool"
  exit 0
fi

download "https://flash.android.com" "flash.html"
FLASH_KEY=$(grep -o '<body data-client-config=.*' "flash.html" 2>/dev/null | cut -d\; -f2 | cut -d\& -f1)
[ -z "$FLASH_KEY" ] && { log "could not obtain flash API key — keeping bundled pool"; exit 0; }

download "https://source.android.com/docs/security/bulletin/pixel" "secbull.html"

resolve_persona() {
  _idx="$1"
  _model=$(printf '%s\n'  "$MODEL_LIST"   | sed -n "${_idx}p")
  _device=$(printf '%s\n' "$PRODUCT_LIST" | sed -n "${_idx}p")
  [ -z "$_model" ] && return 1
  [ -z "$_device" ] && return 1
  _platform=$(map_platform "$_device")
  [ -z "$_platform" ] && return 1

  _station_url="https://content-flashstation-pa.googleapis.com/v1/builds?product=${_device}_beta&key=$FLASH_KEY"
  download "$_station_url" "station.json" "https://flash.android.com"
  [ -s "station.json" ] || { log "skip $_model: no build data"; return 1; }

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

  _canary_id=$(grep '"id"' "canary.json" | sed -e 's;.*canary-\(.*\)";\1;' -e 's;^\(.\{4\}\);\1-;' | head -n1)
  _spatch=""
  if [ -n "$_canary_id" ] && [ -s "secbull.html" ]; then
    _spatch=$(grep "<td>$_canary_id" "secbull.html" 2>/dev/null | sed 's;.*<td>\(.*\)</td>;\1;' | head -n1)
  fi
  [ -z "$_spatch" ] && [ -n "$_canary_id" ] && _spatch="${_canary_id}-05"
  [ -z "$_spatch" ] && { log "skip $_model: no security patch"; return 1; }

  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$_model" "$_device" "$_device" "$_device" "$_platform" \
    "$CANARY_SDK" "$CANARY_RELEASE" "$_bid" "$_incr" "$_spatch"
  return 0
}

start=$(rand_below "$kn")
persona_line=""
try=0
row=""
while [ "$try" -lt "$kn" ] && [ "$try" -lt "$MAX_TRY" ]; do
  off=$(( (start + try) % kn ))
  try=$((try + 1))
  eval "row=\${$((off + 1))}"
  persona_line=$(resolve_persona "$row")
  [ -n "$persona_line" ] && break
done

if [ -z "$persona_line" ]; then
  log "no canary persona resolved in $try tr(y|ies) — keeping bundled pool"
  exit 0
fi

if printf '%s\n' "$persona_line" > "$OVERRIDE_FILE.tmp.$$" 2>/dev/null \
   && mv -f "$OVERRIDE_FILE.tmp.$$" "$OVERRIDE_FILE" 2>/dev/null; then
  chmod 0644 "$OVERRIDE_FILE" 2>/dev/null
  _m=$(printf '%s' "$persona_line" | cut -f1)
  _d=$(printf '%s' "$persona_line" | cut -f2)
  _b=$(printf '%s' "$persona_line" | cut -f8)
  log "fetched random canary persona: $_m ($_d) build $_b — freshen will apply it directly"
else
  rm -f "$OVERRIDE_FILE.tmp.$$" 2>/dev/null
  log "could not write persona override — keeping bundled pool"
fi

exit 0
