#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
JNI_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DEST="$JNI_DIR/zygisk.hpp"

ZYGISK_HPP_COMMIT="8ce26128f81baaed0b969aaf7f52f886b61af4ab"
ZYGISK_HPP_SHA256="f8d55e8b4f89d418c5941afe62ce6a09ddec1f4afd9a1b0a01eb40a93310dd28"
ZYGISK_HPP_URL="https://raw.githubusercontent.com/topjohnwu/zygisk-module-sample/${ZYGISK_HPP_COMMIT}/module/jni/zygisk.hpp"

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -d' ' -f1
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | cut -d' ' -f1
  else
    echo "ERROR: no sha256 tool (sha256sum/shasum) to verify zygisk.hpp" >&2
    return 2
  fi
}

if [ ! -f "$DEST" ]; then
  if ! command -v curl >/dev/null 2>&1; then
    echo "ERROR: curl not found and $DEST is not cached" >&2
    echo "  (curl is required to fetch the pinned Zygisk API header)" >&2
    exit 1
  fi
  echo "==> Fetching zygisk.hpp @ ${ZYGISK_HPP_COMMIT}"
  if ! curl -fsSL -o "$DEST" "$ZYGISK_HPP_URL"; then
    echo "ERROR: failed to fetch zygisk.hpp from pinned commit ${ZYGISK_HPP_COMMIT}" >&2
    echo "  check network access to raw.githubusercontent.com" >&2
    rm -f "$DEST"
    exit 1
  fi
fi

GOT="$(sha256_of "$DEST")"
if [ "$GOT" != "$ZYGISK_HPP_SHA256" ]; then
  echo "ERROR: zygisk.hpp checksum mismatch — refusing to continue" >&2
  echo "  expected $ZYGISK_HPP_SHA256" >&2
  echo "  got      $GOT" >&2
  echo "  delete $DEST to re-fetch from pinned commit ${ZYGISK_HPP_COMMIT}" >&2
  exit 1
fi
echo "==> zygisk.hpp verified"
