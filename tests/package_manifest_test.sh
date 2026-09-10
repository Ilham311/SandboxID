#!/usr/bin/env bash
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
MODE=${1:---source}
checks=0
failures=0

check() {
  checks=$((checks + 1))
  if ! eval "$1"; then
    failures=$((failures + 1))
    printf 'FAIL: %s\n' "$2"
  fi
}

mandatory_runtime=(
  module.prop action.sh service.sh customize.sh post-fs-data.sh helpers.sh
  rotate_ids.sh selftest.sh autopif.sh target.txt webroot/index.html
  webroot/app.js webroot/style.css webroot/theme-init.js
)

if [ "$MODE" = --source ]; then
  for file in "${mandatory_runtime[@]}"; do
    if [ "$file" = module.prop ]; then
      continue
    fi
    check '[ -f "$ROOT/$file" ]' "source runtime file missing or not regular: $file"
  done
  check '! grep -q "devices.tsv" "$ROOT/build.sh"' \
    'build still packages obsolete devices.tsv'
  check '! grep -q "AUTOPIF_REFRESH" "$ROOT/build.sh"' \
    'build still supports network persona refresh'
  check '! grep -q "carriers.tsv" "$ROOT/build.sh"' \
    'build still packages retired carriers.tsv'
  check '! grep -q "carrier.conf" "$ROOT/build.sh"' \
    'build still packages retired carrier state'
  check '! grep -q "SBX_SYSFS_MAC" "$ROOT/customize.sh"' \
    'installer still restores retired sysfs-MAC flag'
  check 'grep -q "tests/package_manifest_test.sh" "$ROOT/build.sh"' \
    'build does not run the package manifest gate'
  check 'grep -q "for suite in action autopif rotation helpers_applog customize package_manifest" "$ROOT/validate.sh"' \
    'validation does not run the installer preservation suite'
  check 'grep -q "preserve_validated_pair" "$ROOT/customize.sh" && grep -q "persona.cache.meta" "$ROOT/customize.sh" && ! grep -q "enable_remote_refresh" "$ROOT/customize.sh"' \
    'installer does not preserve coherent identity/cache state or still retains the obsolete refresh toggle'
  check 'grep -q "MANDATORY_RUNTIME" "$ROOT/build.sh"' \
    'build does not declare mandatory runtime inputs'
  if [ -e "$ROOT/prebuilt/resetprop-rs" ] || [ -e "$ROOT/prebuilt/resetprop-rs.sha256" ] ||
     [ -e "$ROOT/prebuilt/resetprop-rs.LICENSE" ]; then
    check '[ -f "$ROOT/prebuilt/resetprop-rs" ] && [ -f "$ROOT/prebuilt/resetprop-rs.sha256" ] && [ -f "$ROOT/prebuilt/resetprop-rs.LICENSE" ]' \
      'source resetprop-rs binary/checksum/license set is incomplete'
    check 'grep -q "MIT License" "$ROOT/prebuilt/resetprop-rs.LICENSE" && grep -q "Copyright (c) 2026 Enginex0" "$ROOT/prebuilt/resetprop-rs.LICENSE"' \
      'source resetprop-rs MIT notice is missing or incomplete'
  fi
  if [ ! -f "$ROOT/module.prop" ]; then
    printf 'BLOCKER: module.prop is absent; Android package build remains unavailable\n'
  fi
else
  PKG=$MODE
  for file in "${mandatory_runtime[@]}"; do
    check '[ -f "$PKG/$file" ]' "package runtime file missing or not regular: $file"
  done
  for abi in arm64-v8a armeabi-v7a x86_64 x86; do
    check '[ -s "$PKG/zygisk/$abi.so" ]' "Zygisk library missing: $abi"
  done
  for cli in sandboxid-arm64 sandboxid-arm sandboxid-x86_64 sandboxid-x86; do
    check '[ -s "$PKG/bin/$cli" ]' "native CLI missing: $cli"
  done
  check '[ ! -e "$PKG/devices.tsv" ]' 'obsolete devices.tsv was packaged'
  check '[ ! -e "$PKG/carriers.tsv" ]' 'retired carriers.tsv was packaged'
  check '[ ! -e "$PKG/carrier.conf" ]' 'retired carrier state was packaged'
  if [ -e "$PKG/bin/resetprop-rs" ] || [ -e "$PKG/bin/resetprop-rs.sha256" ] ||
     [ -e "$PKG/bin/resetprop-rs.LICENSE" ]; then
    check '[ -f "$PKG/bin/resetprop-rs" ] && [ -f "$PKG/bin/resetprop-rs.sha256" ] && [ -f "$PKG/bin/resetprop-rs.LICENSE" ]' \
      'resetprop-rs binary/checksum/license set is incomplete'
    check 'grep -q "MIT License" "$PKG/bin/resetprop-rs.LICENSE" && grep -q "Copyright (c) 2026 Enginex0" "$PKG/bin/resetprop-rs.LICENSE"' \
      'resetprop-rs MIT notice is missing or incomplete'
    if command -v sha256sum >/dev/null 2>&1; then
      check '(cd "$PKG/bin" && sha256sum -c resetprop-rs.sha256 >/dev/null 2>&1)' \
        'packaged resetprop-rs checksum verification failed'
    else
      check 'false' 'sha256sum unavailable for packaged resetprop-rs verification'
    fi
  fi
fi

printf '\n%d checks, %d failures\n' "$checks" "$failures"
exit "$failures"
