#!/bin/sh
set -u

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP_ROOT=${SBX_VALIDATE_TMPDIR:-${TMPDIR:-$ROOT/.claude-tmp}}
mkdir -p "$TMP_ROOT" || exit 1
TMP=$(mktemp -d "$TMP_ROOT/sbx-resetprop-test.XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT INT TERM

rc=0
fail() { printf 'FAIL: %s\n' "$*" >&2; rc=1; }

RP_BODY=$(awk '/^rp_set\(\)/,/^}/' "$ROOT/helpers.sh")
printf '%s\n' "$RP_BODY" | grep -Fq '"$MODDIR/bin/resetprop-rs"' || \
    fail "rp_set does not require the bundled executable"
if printf '%s\n' "$RP_BODY" | grep -Eq \
   'command -v[[:space:]]+(resetprop|resetprop-rs|setprop)|(^|[;&|[:space:]])(resetprop|setprop)[[:space:]]'; then
    fail "rp_set contains an external resetprop/setprop fallback"
fi

MOD="$TMP/mod"
TRAPS="$TMP/traps"
CALLS="$TMP/calls"
PATH_CALLS="$TMP/path-calls"
mkdir -p "$MOD/bin" "$TRAPS" "$TMP/backups"
: > "$CALLS"
: > "$PATH_CALLS"
for name in resetprop resetprop-rs setprop; do
    cat > "$TRAPS/$name" <<'EOF'
#!/bin/sh
printf '%s %s\n' "${0##*/}" "$*" >> "${SBX_PATH_CALLS:?}"
exit 0
EOF
    chmod 0755 "$TRAPS/$name"
done

if PATH="$TRAPS:$PATH" MODDIR="$MOD" LOGFILE="$TMP/helpers.log" \
   BACKUP_DIR_ROOT="$TMP/backups" SBX_PATH_CALLS="$PATH_CALLS" \
   sh -c '. "$1"; rp_set ro.sbx.test value' sh "$ROOT/helpers.sh"; then
    fail "rp_set succeeded without the bundled executable"
fi
[ ! -s "$PATH_CALLS" ] || fail "rp_set invoked a PATH fallback"

cat > "$MOD/bin/resetprop-rs" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "${SBX_BUNDLED_CALLS:?}"
EOF
chmod 0755 "$MOD/bin/resetprop-rs"
PATH="$TRAPS:$PATH" MODDIR="$MOD" LOGFILE="$TMP/helpers.log" \
BACKUP_DIR_ROOT="$TMP/backups" SBX_PATH_CALLS="$PATH_CALLS" \
SBX_BUNDLED_CALLS="$CALLS" sh -c \
'. "$1"; rp_set ro.sbx.test value && rp_set persist.sbx.test saved' \
sh "$ROOT/helpers.sh" || fail "bundled resetprop-rs invocation failed"
grep -qx -- '-n ro.sbx.test value' "$CALLS" || fail "non-persistent flag mismatch"
grep -qx -- '-p persist.sbx.test saved' "$CALLS" || fail "persistent flag mismatch"
[ ! -s "$PATH_CALLS" ] || fail "PATH fallback ran alongside bundled backend"

MANIFEST="$ROOT/prebuilt/resetprop-rs.sha256"
EXPECTED="$TMP/expected"
ACTUAL="$TMP/actual"
printf '%s\n' resetprop-arm64-v8a resetprop-armeabi-v7a \
    resetprop-x86_64 resetprop-x86 > "$EXPECTED"
if [ ! -f "$MANIFEST" ]; then
    fail "resetprop-rs checksum manifest is missing"
else
    awk 'NF {print $2}' "$MANIFEST" > "$ACTUAL"
    cmp -s "$EXPECTED" "$ACTUAL" || fail "manifest must contain exactly four required ABI assets"
    bad=$(grep -Evc '^[0-9a-f]{64}  resetprop-(arm64-v8a|armeabi-v7a|x86_64|x86)$' "$MANIFEST")
    [ "$bad" -eq 0 ] || fail "manifest contains malformed checksum entries"
    if command -v sha256sum >/dev/null 2>&1; then
        (cd "$ROOT/prebuilt" && sha256sum -c resetprop-rs.sha256 >/dev/null) || \
            fail "resetprop-rs checksum verification failed"
    else
        fail "sha256sum is required to verify resetprop-rs"
    fi
fi

if command -v file >/dev/null 2>&1; then
    for spec in 'arm64-v8a:ELF 64-bit.*ARM aarch64' \
                'armeabi-v7a:ELF 32-bit.*ARM' \
                'x86_64:ELF 64-bit.*x86-64' \
                'x86:ELF 32-bit.*Intel i386'; do
        abi=${spec%%:*}; pattern=${spec#*:}
        file "$ROOT/prebuilt/resetprop-$abi" | grep -Eq "$pattern" || \
            fail "resetprop-$abi has the wrong ELF architecture"
    done
fi

if [ "$rc" -eq 0 ]; then
    echo "OK: resetprop backend is bundled-only and all four pinned assets verify"
fi
exit "$rc"
