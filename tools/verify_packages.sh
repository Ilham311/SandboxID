#!/usr/bin/env bash
# Verifies the two release zips after the build.
#
# Called by .github/workflows/build.yml. The zip is the only thing a user ever
# installs, and the module.prop inside it is the only thing the update channel
# ever sees, so both are checked rather than trusted:
#
#   - every expected ABI ships a zygisk library, and the CLI binaries are present
#   - module.prop sits at the package root with the resolved version and code
#   - each variant's updateJson points at its OWN channel
#
# That last check is the load-bearing one. update.json carries a single zipUrl,
# so the only way a debug install stays debug across an update is for the debug
# package to read a different channel. If build.sh's rewrite ever regresses, a
# debug device silently installs the release zip on update — this is the check
# that catches it before the zip is published.
#
# Usage: NV=v2.2.8 NC=239 ./tools/verify_packages.sh
set -euo pipefail

NV="${NV:?verify_packages.sh: NV (e.g. v2.2.8) is required}"
NC="${NC:?verify_packages.sh: NC (e.g. 239) is required}"
DIST="${DIST:-dist}"

# ABIs are parsed out of build.sh so this list cannot lag behind a build change.
ABIS="$(sed -n 's/^ABIS=(\(.*\))$/\1/p' build.sh | head -n 1)"
[ -n "$ABIS" ] || ABIS="arm64-v8a armeabi-v7a x86_64 x86"

fail() { echo "::error::$*"; exit 1; }

for V in release debug; do
  ZIP="$DIST/sandboxid-${NV}-${V}.zip"
  [ -f "$ZIP" ] || fail "missing build artifact $ZIP"
  echo "verify: $ZIP ($(du -h "$ZIP" | cut -f1))"

  MANIFEST="$(mktemp)"
  # unzip -p writes nothing and fails when the entry is absent, which is exactly
  # the failure to catch: a package without a manifest is not installable.
  if ! unzip -p "$ZIP" module.prop > "$MANIFEST" 2>/dev/null; then
    rm -f "$MANIFEST"
    fail "$ZIP has no module.prop at its package root"
  fi

  grep -qx "version=${NV}"      "$MANIFEST" || { rm -f "$MANIFEST"; fail "$ZIP module.prop version is not ${NV}"; }
  grep -qx "versionCode=${NC}" "$MANIFEST" || { rm -f "$MANIFEST"; fail "$ZIP module.prop versionCode is not ${NC}"; }

  CHAN="$(grep '^updateJson=' "$MANIFEST" | head -n 1 | cut -d= -f2-)"
  if [ -z "$CHAN" ]; then
    rm -f "$MANIFEST"
    fail "$ZIP module.prop has no updateJson= line — the package cannot receive updates"
  fi

  if [ "$V" = debug ]; then
    # A debug package on the release channel loses its variant on the next update.
    case "$CHAN" in
      */update.debug.json) ;;
      *) rm -f "$MANIFEST"; fail "$ZIP updateJson is '$CHAN' — a debug package must read the debug channel (.../update.debug.json), otherwise an update silently installs the release zip" ;;
    esac
    grep -q '^name=.*\[DEBUG\]' "$MANIFEST" || { rm -f "$MANIFEST"; fail "$ZIP name= lacks the [DEBUG] marker, so the variant is indistinguishable from a release install"; }
  else
    case "$CHAN" in
      */update.json) ;;
      *) rm -f "$MANIFEST"; fail "$ZIP updateJson is '$CHAN' — the release package must read the release channel (.../update.json)" ;;
    esac
    if grep -q '^name=.*\[DEBUG\]' "$MANIFEST"; then
      rm -f "$MANIFEST"
      fail "$ZIP is the release package but its name= carries a [DEBUG] marker"
    fi
  fi
  rm -f "$MANIFEST"

  # A silently-skipped cmake build would still zip fine; it would just
  # force-close on an architecture it does not ship a library for.
  for A in $ABIS; do
    unzip -l "$ZIP" | grep -q "zygisk/${A}\\.so" || fail "$ZIP is missing zygisk/${A}.so"
  done
  unzip -l "$ZIP" | grep -q 'bin/sandboxid' || fail "$ZIP is missing its CLI binaries"
done

echo "verify: both packages match ${NV} (code ${NC}) and read their own update channel"
