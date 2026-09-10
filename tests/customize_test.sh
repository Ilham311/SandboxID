#!/usr/bin/env bash
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
checks=0
failures=0

check() {
  local label=$1
  shift
  checks=$((checks + 1))
  if "$@"; then
    return 0
  fi
  failures=$((failures + 1))
  printf 'FAIL: %s\n' "$label"
  return 1
}

hash_file() {
  sha256sum "$1" | cut -d' ' -f1
}

make_pair() {
  local dir=$1 data=$2 meta=$3 key=$4 body=$5
  printf '%s' "$body" > "$dir/$data"
  printf 'version=1\n%s=%s\n' "$key" "$(hash_file "$dir/$data")" > "$dir/$meta"
}

run_installer() {
  local live=$1 stage=$2 modules=$3 out=$4
  env -i PATH="$PATH" ROOT="$ROOT" MODPATH="$stage" \
    SBX_LIVE_MODULE_DIR="$live" SBX_MODULES_ROOT="$modules" \
    MAGISK_VER_CODE=26100 bash -c '
      set -u
      ui_print() { :; }
      abort() { printf "%s\n" "$*" >&2; exit 90; }
      getprop() { [ "$1" = ro.product.cpu.abi ] && printf "arm64-v8a\n"; }
      set_perm_recursive() { :; }
      set_perm() { chmod "$4" "$1" 2>/dev/null || return 1; }
      . "$ROOT/customize.sh"
    ' >"$out" 2>&1
}

new_fixture() {
  FIXTURE=$(mktemp -d "${TMPDIR:-/tmp}/sbx-installer.XXXXXX") || exit 1
  LIVE="$FIXTURE/live"
  STAGE="$FIXTURE/stage"
  MODULES="$FIXTURE/modules"
  mkdir -p "$LIVE" "$STAGE/bin" "$STAGE/webroot" "$MODULES"
  printf 'version=1\n' > "$STAGE/module.prop"
  for file in action.sh service.sh helpers.sh rotate_ids.sh autopif.sh target.txt; do
    : > "$STAGE/$file"
  done
  for cli in sandboxid-arm64 sandboxid-arm sandboxid-x86_64 sandboxid-x86; do
    printf '#!/system/bin/sh\n' > "$STAGE/bin/$cli"
    chmod 0755 "$STAGE/bin/$cli"
  done
}

cleanup_fixture() {
  rm -rf "$FIXTURE"
}

new_fixture
printf 'com.example.app:worker\n' > "$LIVE/target.txt"
printf 'fresh\n' > "$LIVE/identity.mode"
make_pair "$LIVE" identity.prop identity.meta identity_sha256 $'MODEL=Pixel\nSBX_NATIVE_READ=1\nSBX_SYSFS_MAC=1\n'
make_pair "$LIVE" identity.prop.bak identity.meta.bak identity_sha256 $'MODEL=Pixel-old\n'
make_pair "$LIVE" persona.cache persona.cache.meta candidate_sha256 $'SBX_PERSONA_V1\t35\tcandidate\n'
printf 'legacy carrier\n' > "$LIVE/carrier.conf"
printf 'legacy catalog\n' > "$LIVE/carriers.tsv"
run_installer "$LIVE" "$STAGE" "$MODULES" "$FIXTURE/out"
rc=$?
check 'installer accepts complete validated state pairs' test "$rc" -eq 0
check 'installer preserves canonical pair' cmp -s "$LIVE/identity.prop" "$STAGE/identity.prop"
check 'installer preserves canonical metadata' cmp -s "$LIVE/identity.meta" "$STAGE/identity.meta"
check 'installer preserves backup pair' cmp -s "$LIVE/identity.meta.bak" "$STAGE/identity.meta.bak"
check 'installer preserves remote cache pair' cmp -s "$LIVE/persona.cache.meta" "$STAGE/persona.cache.meta"
check 'installer preserves exact process targets' cmp -s "$LIVE/target.txt" "$STAGE/target.txt"
check 'installer does not preserve retired carrier state' test ! -e "$STAGE/carrier.conf"
check 'installer does not preserve retired carrier catalog' test ! -e "$STAGE/carriers.tsv"
check 'installer does not restore retired sysfs-MAC flag' sh -c '! grep -q "^SBX_SYSFS_MAC=" "$1/.operational-flags" 2>/dev/null' sh "$STAGE"
check 'installer restricts canonical state permissions' test "$(stat -c %a "$STAGE/identity.prop")" = 600
check 'installer restricts cache permissions' test "$(stat -c %a "$STAGE/persona.cache")" = 600
cleanup_fixture

new_fixture
printf 'MODEL=Pixel\n' > "$LIVE/identity.prop"
printf 'version=1\nidentity_sha256=%064d\n' 0 > "$LIVE/identity.meta"
run_installer "$LIVE" "$STAGE" "$MODULES" "$FIXTURE/out"
rc=$?
check 'installer rejects a hash-mismatched canonical pair' test "$rc" -eq 90
cleanup_fixture

new_fixture
printf 'MODEL=Pixel\n' > "$LIVE/identity.prop"
run_installer "$LIVE" "$STAGE" "$MODULES" "$FIXTURE/out"
rc=$?
check 'installer permits legacy identity without metadata' test "$rc" -eq 0
check 'installer preserves legacy identity for native migration' cmp -s "$LIVE/identity.prop" "$STAGE/identity.prop"
check 'installer does not fabricate legacy metadata' test ! -e "$STAGE/identity.meta"
cleanup_fixture

new_fixture
printf 'MODEL=Pixel\n' > "$LIVE/identity.prop"
run_installer "$LIVE" "$STAGE" "$MODULES" "$FIXTURE/out"
rc=$?
check 'control legacy install succeeds' test "$rc" -eq 0
cleanup_fixture

new_fixture
printf 'SBX_PERSONA_V1\t35\tcandidate\n' > "$LIVE/persona.cache"
run_installer "$LIVE" "$STAGE" "$MODULES" "$FIXTURE/out"
rc=$?
check 'installer rejects a half-present persona cache pair' test "$rc" -eq 90
check 'installer does not preserve half-present persona cache data' test ! -e "$STAGE/persona.cache"
cleanup_fixture

new_fixture
printf 'version=1\nidentity_sha256=%064d\n' 0 > "$LIVE/identity.meta"
run_installer "$LIVE" "$STAGE" "$MODULES" "$FIXTURE/out"
rc=$?
check 'installer rejects canonical metadata without identity data' test "$rc" -eq 90
check 'installer does not preserve orphan canonical metadata' test ! -e "$STAGE/identity.meta"
cleanup_fixture

printf '\n%d checks, %d failures\n' "$checks" "$failures"
exit "$failures"
