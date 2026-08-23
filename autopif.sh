#!/system/bin/sh
#
# autopif.sh — SandboxID persona freshener.
#
# Two subcommands:
#
#   fetch    (default)  Online, best-effort: pull ONE fresh RANDOM Pixel canary
#                       persona from Google's live build data and drop it as a
#                       one-shot persona.override that `sandboxid freshen`
#                       consumes directly. No-op (exit 0) when the device has no
#                       curl/wget — freshen then falls back to personas.tsv.
#                       This is what action.sh invokes; behaviour is unchanged.
#
#   device   (offline)  Pick ONE random REAL non-Pixel handset from devices.tsv
#                       (Samsung/Xiaomi/Redmi/POCO/vivo/OPPO/Infinix), derive a
#                       complete, internally-consistent Android identity for it
#                       (fingerprint, build props, random serial/AndroidID/GAID)
#                       plus a coherent device-lifecycle profile — boot count,
#                       uptime and a "fresh" usage indicator whose values are
#                       validated to agree with the model's age — then DISPLAY
#                       it and write an apply-ready artifact (device.identity).
#
#                       NOTE: the native `freshen`/persona-override path is
#                       hardwired to Google/Pixel/Tensor (derive_identity() in
#                       sandboxid.cpp forces BRAND=google + a Tensor SoC/modem
#                       mapping, and parse_persona_line() rejects non-Tensor
#                       platforms). So `device` does NOT feed persona.override.
#                       Its artifact is a full identity.prop-format file; apply
#                       it live with:  cp device.identity identity.prop &&
#                       sandboxid apply-boot   (opt-in; see README follow-up).
#
# Aliases for `device`: gen, multibrand, profile.

MODDIR="${MODDIR:-/data/adb/modules/sandboxid}"
OVERRIDE_FILE="${PERSONA_OVERRIDE:-$MODDIR/persona.override}"

# fetch (Pixel canary) knobs
CANARY_RELEASE="${CANARY_RELEASE:-16}"
CANARY_SDK="${CANARY_SDK:-36}"
MAX_TRY="${AUTOPIF_MAX_TRY:-4}"

# device (multi-brand) knobs
DEVICES_FILE="${AUTOPIF_DEVICES:-$MODDIR/devices.tsv}"
IDENTITY_ARTIFACT="${AUTOPIF_ARTIFACT:-$MODDIR/device.identity}"
GEN_MAX_TRY="${AUTOPIF_GEN_MAX_TRY:-6}"

log() { echo "[autopif] $*"; }

# --------------------------------------------------------------------------
# shared helpers
# --------------------------------------------------------------------------

# uniform integer in [0, n)
rand_below() {
  _n="$1"
  [ "$_n" -gt 0 ] 2>/dev/null || { echo 0; return; }
  _h=$(od -An -N4 -tx1 /dev/urandom 2>/dev/null | tr -d ' \n')
  if [ -n "$_h" ]; then _r=$(( (0x$_h) & 0x7fffffff )); else _r=$(( ($$ * 2654435761) & 0x7fffffff )); fi
  echo $(( _r % _n ))
}

# uniform integer in [lo, hi] inclusive
rand_range() {
  _lo="$1"; _hi="$2"
  [ "$_hi" -le "$_lo" ] 2>/dev/null && { echo "$_lo"; return; }
  echo $(( _lo + $(rand_below $(( _hi - _lo + 1 )) ) ))
}

# lowercase 2*nbytes hex string from /dev/urandom (fallback: pid-seeded)
rand_hex() {
  _nb="$1"
  _x=$(od -An -N"$_nb" -tx1 /dev/urandom 2>/dev/null | tr -d ' \n')
  if [ -z "$_x" ]; then
    _x=""
    _seed=$$
    _i=0
    while [ "$_i" -lt "$_nb" ]; do
      _seed=$(( (_seed * 1103515245 + 12345) & 0x7fffffff ))
      _x="$_x$(printf '%02x' $(( _seed & 0xff )))"
      _i=$(( _i + 1 ))
    done
  fi
  printf '%s' "$_x"
}

# RFC-4122 v4 UUID (lowercase) — GAID/AAID shape
rand_uuid() {
  _a=$(rand_hex 4); _b=$(rand_hex 2); _c=$(rand_hex 2); _d=$(rand_hex 2); _e=$(rand_hex 6)
  _c="4$(printf '%s' "$_c" | cut -c2-4)"                       # version 4
  _v=$(printf '89ab' | cut -c$(( $(rand_below 4) + 1 )))       # variant 10xx
  _d="${_v}$(printf '%s' "$_d" | cut -c2-4)"
  printf '%s-%s-%s-%s-%s' "$_a" "$_b" "$_c" "$_d" "$_e"
}

# strip a single leading zero so dash/mksh don't read "08"/"09" as bad octal
strip0() { case "$1" in 0?) echo "${1#0}" ;; *) echo "$1" ;; esac; }

# days since 1970-01-01 for Y M D (proleptic Gregorian, Howard Hinnant algo).
# All inputs here are post-1970 so year/era are non-negative and the reference
# algorithm's negative-branch ternaries are dead — omitted to stay POSIX.
days_from_civil() {
  _y="$1"; _m="$2"; _d="$3"
  [ "$_m" -le 2 ] && _y=$(( _y - 1 ))
  _era=$(( _y / 400 ))
  _yoe=$(( _y - _era * 400 ))
  if [ "$_m" -gt 2 ]; then _mp=$(( _m - 3 )); else _mp=$(( _m + 9 )); fi
  _doy=$(( (153 * _mp + 2) / 5 + _d - 1 ))
  _doe=$(( _yoe * 365 + _yoe / 4 - _yoe / 100 + _doy ))
  echo $(( _era * 146097 + _doe - 719468 ))
}

# epoch (UTC 00:00) for YYYY-MM or YYYY-MM-DD (missing day => mid-month 15th)
ymd_to_epoch() {
  _y=$(echo "$1" | cut -d- -f1)
  _mo=$(echo "$1" | cut -d- -f2)
  _da=$(echo "$1" | cut -d- -f3)
  [ -z "$_da" ] && _da=15
  _mo=$(strip0 "$_mo"); _da=$(strip0 "$_da")
  echo $(( $(days_from_civil "$_y" "$_mo" "$_da") * 86400 ))
}

# epoch -> YYYY-MM-DD (UTC). z is always > 0 for our dates -> no negative branch.
epoch_to_ymd() {
  _z=$(( $1 / 86400 + 719468 ))
  _era=$(( _z / 146097 ))
  _doe=$(( _z - _era * 146097 ))
  _yoe=$(( (_doe - _doe / 1460 + _doe / 36524 - _doe / 146096) / 365 ))
  _y=$(( _yoe + _era * 400 ))
  _doy=$(( _doe - (365 * _yoe + _yoe / 4 - _yoe / 100) ))
  _mp=$(( (5 * _doy + 2) / 153 ))
  _d=$(( _doy - (153 * _mp + 2) / 5 + 1 ))
  if [ "$_mp" -lt 10 ]; then _mo=$(( _mp + 3 )); else _mo=$(( _mp - 9 )); fi
  [ "$_mo" -le 2 ] && _y=$(( _y + 1 ))
  printf '%04d-%02d-%02d' "$_y" "$_mo" "$_d"
}

# seconds -> "Xd Yh Zm"
fmt_dur() {
  _s="$1"
  printf '%dd %dh %dm' $(( _s / 86400 )) $(( (_s % 86400) / 3600 )) $(( (_s % 3600) / 60 ))
}

# now (epoch). AUTOPIF_NOW overrides for reproducible tests.
now_epoch() {
  if [ -n "$AUTOPIF_NOW" ]; then echo "$AUTOPIF_NOW"; return; fi
  _n=$(date +%s 2>/dev/null)
  case "$_n" in ''|*[!0-9]*) _n=$(ymd_to_epoch "2025-01-01") ;; esac
  echo "$_n"
}

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

make_tmp() {
  TMP_DIR=$(mktemp -d 2>/dev/null)
  if [ -z "$TMP_DIR" ] || [ ! -d "$TMP_DIR" ]; then
    TMP_DIR="${TMPDIR:-/data/local/tmp}/autopif.$$"
    [ -e "$TMP_DIR" ] && return 1        # refuse to reuse/pre-existing path
    mkdir "$TMP_DIR" 2>/dev/null || return 1
  fi
  return 0
}

# --------------------------------------------------------------------------
# subcommand: fetch  (online Pixel canary -> persona.override)  [unchanged]
# --------------------------------------------------------------------------

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

  # The canary build "id" embeds the security-patch month as YYYYMM right after
  # "canary-". Isolate the tail after the last "canary-", take the first 6-digit
  # run, and format YYYY-MM. Rejecting non-6-digit input keeps a malformed id
  # (which previously leaked a stray-comma date like "2026-08,-05" into
  # persona.override) from ever reaching freshen.
  _canary_tail=$(grep '"id"' "canary.json" | sed 's;.*canary-;;' | head -n1)
  _canary_ym=$(printf '%s' "$_canary_tail" | sed 's;^\([0-9]\{6\}\).*;\1;')
  case "$_canary_ym" in
    [0-9][0-9][0-9][0-9][0-9][0-9])
      _canary_id="$(printf '%s' "$_canary_ym" | cut -c1-4)-$(printf '%s' "$_canary_ym" | cut -c5-6)" ;;
    *) _canary_id="" ;;
  esac
  _spatch=""
  if [ -n "$_canary_id" ] && [ -s "secbull.html" ]; then
    _spatch=$(grep "<td>$_canary_id" "secbull.html" 2>/dev/null | sed 's;.*<td>\(.*\)</td>;\1;' | head -n1)
  fi
  [ -z "$_spatch" ] && [ -n "$_canary_id" ] && _spatch="${_canary_id}-05"
  # final guard: only accept a well-formed YYYY-MM-DD security patch
  case "$_spatch" in
    [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]) : ;;
    *) log "skip $_model: unparseable security patch '$_spatch'"; return 1 ;;
  esac

  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$_model" "$_device" "$_device" "$_device" "$_platform" \
    "$CANARY_SDK" "$CANARY_RELEASE" "$_bid" "$_incr" "$_spatch"
  return 0
}

cmd_fetch() {
  rm -f "$OVERRIDE_FILE" 2>/dev/null

  DL=""
  if command -v curl >/dev/null 2>&1; then
    DL="curl"
  elif command -v wget >/dev/null 2>&1; then
    DL="wget"
  else
    log "no curl/wget on device — freshen will use bundled personas.tsv (offline no-op)"
    return 0
  fi

  make_tmp || { log "no writable temp dir — aborting refresh"; return 0; }
  cleanup() { rm -rf "$TMP_DIR" 2>/dev/null; }
  trap cleanup EXIT
  cd "$TMP_DIR" 2>/dev/null || { log "cannot enter temp dir — aborting"; return 0; }

  download "https://developer.android.com/about/versions" "versions.html"
  LATEST_URL=$(grep -o 'https://developer.android.com/about/versions/.*[0-9]"' "versions.html" 2>/dev/null | sed 's;.*/\([0-9][0-9]*\)"$;\1 &;' | sort -rn | cut -d' ' -f2- | cut -d\" -f1 | head -n1)
  [ -z "$LATEST_URL" ] && { log "could not resolve latest versions URL — keeping bundled pool"; return 0; }
  download "$LATEST_URL" "latest.html"

  FI_PATH=$(grep -o 'href=".*download.*"' "latest.html" 2>/dev/null | grep 'qpr' | cut -d\" -f2 | head -n1)
  [ -z "$FI_PATH" ] && { log "no factory-image link found — keeping bundled pool"; return 0; }
  download "https://developer.android.com$FI_PATH" "fi.html"

  MODEL_LIST=$(grep -A1 'tr id=' "fi.html" 2>/dev/null | grep 'td' | sed 's;.*<td>\(.*\)</td>.*;\1;')
  PRODUCT_LIST=$(grep 'tr id=' "fi.html" 2>/dev/null | sed 's;.*<tr id="\(.*\)">.*;\1;')

  count_model=$(printf '%s\n' "$MODEL_LIST"   | grep -c .)
  count_prod=$(printf '%s\n'  "$PRODUCT_LIST" | grep -c .)
  if [ "$count_model" -eq 0 ] || [ "$count_model" -ne "$count_prod" ]; then
    log "device table parse failed (models=$count_model products=$count_prod) — keeping bundled pool"
    return 0
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
    return 0
  fi

  download "https://flash.android.com" "flash.html"
  FLASH_KEY=$(grep -o '<body data-client-config=.*' "flash.html" 2>/dev/null | cut -d\; -f2 | cut -d\& -f1)
  [ -z "$FLASH_KEY" ] && { log "could not obtain flash API key — keeping bundled pool"; return 0; }

  download "https://source.android.com/docs/security/bulletin/pixel" "secbull.html"

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
    return 0
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

  return 0
}

# --------------------------------------------------------------------------
# subcommand: device  (offline multi-brand identity + lifecycle generator)
# --------------------------------------------------------------------------

# expected Android release for a given SDK level (consistency guard)
sdk_release() {
  case "$1" in
    30) echo "11" ;; 31) echo "12" ;; 32) echo "12" ;;
    33) echo "13" ;; 34) echo "14" ;; 35) echo "15" ;; 36) echo "16" ;;
    *)  echo "" ;;
  esac
}

col() { printf '%s' "$1" | cut -f"$2"; }

# derive coherent boot count / uptime / fresh status from the model's launch date.
# Sets globals: AGE_DAYS OWNED_DAYS BOOT_COUNT UPTIME_S FIRST_BOOT LAST_BOOT_EP
#               FRESH PROFILE DPB10 RESET
gen_lifecycle() {
  _rel="$1"; _now="$2"
  _rel_ep=$(ymd_to_epoch "$_rel")
  _age_days=$(( (_now - _rel_ep) / 86400 ))
  [ "$_age_days" -lt 1 ] && _age_days=$(rand_range 1 30)     # future/at-launch => brand new
  [ "$_age_days" -gt 1825 ] && _age_days=1825                # cap absurd mileage (~5y)

  # ~18% recently factory-reset (resale/repair/fresh identity — this module's own
  # scenario): in-service time collapses to days and BOOT_COUNT restarts low.
  _reset=0
  if [ "$(rand_below 100)" -lt 18 ]; then
    _reset=1
    _owned_days=$(rand_range 1 25)
    [ "$_owned_days" -gt "$_age_days" ] && _owned_days=$_age_days
  elif [ "$_age_days" -le 45 ]; then
    _owned_days=$(rand_range 1 "$_age_days")
  else
    _pct=$(rand_range 50 100)                                 # owned 50–100% of model life
    _owned_days=$(( _age_days * _pct / 100 ))
    [ "$_owned_days" -lt 1 ] && _owned_days=1
  fi

  _dpb10=$(rand_range 30 100)                                 # 3.0–10.0 days/boot (x10)
  _setup_boots=$(rand_range 2 5)                              # first-setup + early OTA boots
  _boot_count=$(( _owned_days * 10 / _dpb10 + _setup_boots ))
  [ "$_boot_count" -lt 1 ] && _boot_count=1
  [ "$_boot_count" -gt 1500 ] && _boot_count=1500

  _dpb=$(( _dpb10 / 10 )); [ "$_dpb" -lt 1 ] && _dpb=1
  _up_cap_days=$(( _dpb * 3 / 2 + 1 ))
  [ "$_up_cap_days" -gt "$_owned_days" ] && _up_cap_days=$_owned_days
  [ "$_up_cap_days" -gt 21 ] && _up_cap_days=21
  [ "$_up_cap_days" -lt 1 ] && _up_cap_days=1
  _up_max_s=$(( _up_cap_days * 86400 ))
  _a=$(rand_range 1200 "$_up_max_s"); _b=$(rand_range 1200 "$_up_max_s")
  if [ "$_a" -le "$_b" ]; then _uptime_s=$_a; else _uptime_s=$_b; fi   # skew short

  _first_boot_ep=$(( _now - _owned_days * 86400 ))
  _last_boot_ep=$(( _now - _uptime_s ))

  if [ "$_reset" -eq 1 ] || [ "$_owned_days" -lt 21 ] || [ "$_boot_count" -lt 8 ]; then
    _fresh="yes"; _profile="fresh"
  elif [ "$_owned_days" -gt 540 ] || [ "$_boot_count" -gt 160 ]; then
    _fresh="no"; _profile="seasoned"
  else
    _fresh="no"; _profile="active"
  fi

  AGE_DAYS=$_age_days; OWNED_DAYS=$_owned_days; BOOT_COUNT=$_boot_count
  UPTIME_S=$_uptime_s; FIRST_BOOT=$(epoch_to_ymd "$_first_boot_ep")
  LAST_BOOT_EP=$_last_boot_ep; FRESH=$_fresh; PROFILE=$_profile
  DPB10=$_dpb10; RESET=$_reset
}

# validate the lifecycle invariants; on failure appends reasons to ERRMSG and
# returns nonzero. Reads globals set by gen_lifecycle. Call DIRECTLY (not in a
# command substitution) so it observes the parent shell's globals.
validate_lifecycle() {
  _now="$1"; _f=0
  [ "$AGE_DAYS" -ge 1 ] || { ERRMSG="$ERRMSG bad-age($AGE_DAYS)"; _f=1; }
  { [ "$OWNED_DAYS" -ge 1 ] && [ "$OWNED_DAYS" -le "$AGE_DAYS" ]; } || { ERRMSG="$ERRMSG owned-not-in-[1,age]"; _f=1; }
  [ "$BOOT_COUNT" -ge 1 ] || { ERRMSG="$ERRMSG boot<1"; _f=1; }
  _os=$(( OWNED_DAYS * 86400 ))
  { [ "$UPTIME_S" -ge 1 ] && [ "$UPTIME_S" -le "$_os" ]; } || { ERRMSG="$ERRMSG uptime-not-in-[1,owned]"; _f=1; }
  [ "$UPTIME_S" -le 1814400 ] || { ERRMSG="$ERRMSG uptime>21d"; _f=1; }
  [ "$LAST_BOOT_EP" -ge "$(( _now - _os ))" ] || { ERRMSG="$ERRMSG last_boot<first_boot"; _f=1; }
  return $_f
}

# build the full identity KV (native identity.prop vocabulary) into IDENTITY_KV,
# from a devices.tsv row. On error appends the reason to ERRMSG and returns
# nonzero. Sets many globals -> MUST be called directly, not inside $(...).
assemble_identity() {
  _row="$1"
  BRAND=$(col "$_row" 1);        MANUFACTURER=$(col "$_row" 2)
  MARKETNAME=$(col "$_row" 3);   MODEL=$(col "$_row" 4)
  DEVICE=$(col "$_row" 5);       PRODUCT=$(col "$_row" 6)
  BOARD=$(col "$_row" 7);        SOC_MANUF=$(col "$_row" 8)
  SOC_MODEL=$(col "$_row" 9);    SDK=$(col "$_row" 10)
  RELEASE=$(col "$_row" 11);     BUILD_ID=$(col "$_row" 12)
  INCREMENTAL=$(col "$_row" 13); SECPATCH=$(col "$_row" 14)
  RELEASE_DATE=$(col "$_row" 15)

  for _v in "$BRAND" "$MANUFACTURER" "$MARKETNAME" "$MODEL" "$DEVICE" "$PRODUCT" \
            "$BOARD" "$SOC_MANUF" "$SOC_MODEL" "$SDK" "$RELEASE" "$BUILD_ID" \
            "$INCREMENTAL" "$SECPATCH" "$RELEASE_DATE"; do
    [ -n "$_v" ] || { ERRMSG="$ERRMSG empty-required-field"; return 1; }
  done
  case "$SDK" in ''|*[!0-9]*) ERRMSG="$ERRMSG non-numeric-sdk"; return 1 ;; esac
  case "$SECPATCH" in
    [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]) : ;;
    *) ERRMSG="$ERRMSG secpatch-not-YYYY-MM-DD"; return 1 ;;
  esac
  _exp=$(sdk_release "$SDK")
  if [ -n "$_exp" ] && [ "$_exp" != "$RELEASE" ]; then
    ERRMSG="$ERRMSG sdk$SDK-expects-rel$_exp-got$RELEASE"; return 1
  fi

  FINGERPRINT="$BRAND/$PRODUCT/$DEVICE:$RELEASE/$BUILD_ID/$INCREMENTAL:user/release-keys"
  DESCRIPTION="$PRODUCT-user $RELEASE $BUILD_ID $INCREMENTAL release-keys"
  SERIAL=$(rand_hex 8)
  ANDROID_ID=$(rand_hex 8)
  GAID=$(rand_uuid)
  HOSTN="$(printf '%s' "$BRAND" | tr '[:upper:]' '[:lower:]')-build-$(rand_range 100 999)"

  # native reads these keys back verbatim (load_identity) and applies them; extra
  # keys (BOOT_COUNT, UPTIME_*, ...) are ignored by apply_native, safe as metadata.
  IDENTITY_KV=$(cat <<EOF
BRAND=$BRAND
MANUFACTURER=$MANUFACTURER
MODEL=$MODEL
MARKETNAME=$MARKETNAME
DEVICE=$DEVICE
PRODUCT=$PRODUCT
BOARD=$BOARD
HARDWARE=$BOARD
BOARD_PLATFORM=$BOARD
SOC_MANUFACTURER=$SOC_MANUF
SOC_MODEL=$SOC_MODEL
FINGERPRINT=$FINGERPRINT
ID=$BUILD_ID
DISPLAY=$BUILD_ID
DESCRIPTION=$DESCRIPTION
BOOTLOADER=unknown
HOST=$HOSTN
USER=builder
TYPE=user
TAGS=release-keys
INCREMENTAL=$INCREMENTAL
RELEASE=$RELEASE
SDK_INT=$SDK
SECURITY_PATCH=$SECPATCH
SERIAL=$SERIAL
ANDROID_ID=$ANDROID_ID
GOOGLE_AID=$GAID
RELEASE_DATE=$RELEASE_DATE
AGE_DAYS=$AGE_DAYS
OWNED_DAYS=$OWNED_DAYS
BOOT_COUNT=$BOOT_COUNT
UPTIME_SECONDS=$UPTIME_S
UPTIME_HUMAN=$(fmt_dur "$UPTIME_S")
FIRST_BOOT=$FIRST_BOOT
LAST_BOOT=$(epoch_to_ymd "$LAST_BOOT_EP")
USAGE_PROFILE=$PROFILE
FRESH=$FRESH
EOF
)
  return 0
}

display_profile() {
  echo ""
  echo "  ┌─ SandboxID device profile ─────────────────────────────"
  printf '  │ %-13s %s\n' "Brand"        "$BRAND"
  printf '  │ %-13s %s\n' "Manufacturer" "$MANUFACTURER"
  printf '  │ %-13s %s (%s)\n' "Model"    "$MARKETNAME" "$MODEL"
  printf '  │ %-13s %s / %s\n' "Codename" "$DEVICE" "$PRODUCT"
  printf '  │ %-13s %s %s (%s)\n' "SoC"    "$SOC_MANUF" "$SOC_MODEL" "$BOARD"
  printf '  │ %-13s Android %s (SDK %s), patch %s\n' "OS" "$RELEASE" "$SDK" "$SECPATCH"
  printf '  │ %-13s %s\n' "Fingerprint" "$FINGERPRINT"
  echo "  ├─ lifecycle (coherent w/ $RELEASE_DATE launch) ──────────"
  printf '  │ %-13s %s  (~1 reboot / %s.%sd)\n' "Boot count"  "$BOOT_COUNT" "$(( DPB10 / 10 ))" "$(( DPB10 % 10 ))"
  printf '  │ %-13s %s  (%ss)\n' "Uptime"  "$(fmt_dur "$UPTIME_S")" "$UPTIME_S"
  printf '  │ %-13s %s\n' "First boot"  "$FIRST_BOOT"
  printf '  │ %-13s %s  (age %sd, owned %sd)\n' "Usage"  "$PROFILE" "$AGE_DAYS" "$OWNED_DAYS"
  printf '  │ %-13s %s%s\n' "Fresh"  "$FRESH" "$( [ "$RESET" -eq 1 ] && echo '  (recently factory-reset)' )"
  echo "  ├─ random per-identity ──────────────────────────────────"
  printf '  │ %-13s %s\n' "Serial"      "$SERIAL"
  printf '  │ %-13s %s\n' "Android ID"  "$ANDROID_ID"
  printf '  │ %-13s %s\n' "GAID"        "$GAID"
  echo "  └─────────────────────────────────────────────────────────"
  echo ""
}

cmd_device() {
  NOW=$(now_epoch)

  if [ ! -r "$DEVICES_FILE" ]; then
    log "no device DB at $DEVICES_FILE — nothing to generate"
    return 0
  fi
  make_tmp || { log "no writable temp dir — aborting"; return 0; }
  cleanup_dev() { rm -rf "$TMP_DIR" 2>/dev/null; }
  trap cleanup_dev EXIT

  RAW="$TMP_DIR/devices.raw"
  grep -v '^[[:space:]]*#' "$DEVICES_FILE" 2>/dev/null | grep -v '^[[:space:]]*$' > "$RAW"
  total=$(wc -l < "$RAW" 2>/dev/null | tr -d ' ')
  case "$total" in ''|*[!0-9]*) total=0 ;; esac
  if [ "$total" -eq 0 ]; then
    log "device DB empty after filtering — nothing to generate"
    return 0
  fi

  try=0
  ok=0
  while [ "$try" -lt "$GEN_MAX_TRY" ]; do
    try=$(( try + 1 ))
    idx=$(( $(rand_below "$total") + 1 ))
    row=$(sed -n "${idx}p" "$RAW")
    RELEASE_DATE_PRE=$(col "$row" 15)
    [ -z "$RELEASE_DATE_PRE" ] && continue
    gen_lifecycle "$RELEASE_DATE_PRE" "$NOW"
    # NOTE: assemble_identity / validate_lifecycle set globals, so they MUST run
    # in THIS shell. Calling them inside $(...) would discard every assignment.
    ERRMSG=""
    if ! assemble_identity "$row"; then
      log "row $idx invalid:$ERRMSG — retrying"; continue
    fi
    ERRMSG=""
    if validate_lifecycle "$NOW"; then
      ok=1; break
    else
      log "row $idx lifecycle incoherent:$ERRMSG — retrying"
    fi
  done

  if [ "$ok" -ne 1 ]; then
    log "could not generate a consistent profile in $try tries — aborting"
    return 1
  fi

  display_profile

  if [ "${AUTOPIF_NO_WRITE:-0}" = "1" ]; then
    log "AUTOPIF_NO_WRITE=1 — not writing artifact"
    return 0
  fi
  if printf '%s\n' "$IDENTITY_KV" > "$IDENTITY_ARTIFACT.tmp.$$" 2>/dev/null \
     && mv -f "$IDENTITY_ARTIFACT.tmp.$$" "$IDENTITY_ARTIFACT" 2>/dev/null; then
    chmod 0644 "$IDENTITY_ARTIFACT" 2>/dev/null
    log "wrote apply-ready identity -> $IDENTITY_ARTIFACT"
    log "apply live (opt-in): cp '$IDENTITY_ARTIFACT' '$MODDIR/identity.prop' && '$MODDIR/bin/sandboxid' apply-boot"
  else
    rm -f "$IDENTITY_ARTIFACT.tmp.$$" 2>/dev/null
    log "could not write artifact $IDENTITY_ARTIFACT"
  fi
  return 0
}

# --------------------------------------------------------------------------
# dispatch
# --------------------------------------------------------------------------
case "${1:-fetch}" in
  fetch|"")                    cmd_fetch ;;
  device|gen|multibrand|profile) cmd_device ;;
  *) log "unknown subcommand '$1' (use: fetch | device)"; exit 2 ;;
esac
exit $?
