#!/usr/bin/env bash
# Ternak TT v1.2.0 patch script.
#
# Sandbox lost network + filesystem between v1.1.8 and v1.2.0, so this
# release is delivered as an OVERLAY: full-rewrite files sit next to this
# script and get copied over your v1.1.8 checkout, plus surgical sed edits
# for CHANGELOG.md, customize.sh, build.sh, and .github/workflows/build.yml.
#
# Usage (from your Ilham311/Tt repo checkout root):
#   unzip -o /path/to/ternak-tt-v1.2.0-overlay.zip -d /tmp/tt-v1.2.0
#   cd /path/to/your/Tt/checkout
#   bash /tmp/tt-v1.2.0/apply-v1.2.0.sh /tmp/tt-v1.2.0
#   git add -A && git commit -m 'chore(release): v1.2.0 — ShadowHook AAR + patchelf SONAME'
#   git push
#
# The GitHub Actions workflow will then auto-tag, build, and publish v1.2.0.

set -euo pipefail

SRC="${1:-$(cd "$(dirname "$0")" && pwd)}"
DST="$(pwd)"

if [ ! -f "$DST/module.prop" ] || [ ! -f "$DST/build.sh" ]; then
    echo "!! Run this from the Ternak TT repo root (where module.prop + build.sh live)." >&2
    echo "   Current dir: $DST" >&2
    exit 1
fi

echo "==> Applying Ternak TT v1.2.0 overlay from: $SRC"
echo "==> Onto repo checkout at:                  $DST"

# 1) Full-rewrite files (overwrite as-is)
for f in fetch_lsplant.sh jni/CMakeLists.txt module.prop update.json build.sh .github/workflows/build.yml; do
    if [ -f "$SRC/$f" ]; then
        mkdir -p "$DST/$(dirname "$f")"
        cp -f "$SRC/$f" "$DST/$f"
        echo "   [replaced] $f"
    else
        echo "   [missing in overlay] $f — skipping" >&2
    fi
done
chmod +x "$DST/fetch_lsplant.sh" "$DST/build.sh" 2>/dev/null || true

# 2) customize.sh header update (surgical sed)
if [ -f "$DST/customize.sh" ]; then
    if grep -q 'Ternak TT v1.1.7' "$DST/customize.sh"; then
        # Replace the v1.1.7 header block with v1.2.0 explanation
        python3 - "$DST/customize.sh" <<'PY'
import sys, re
p = sys.argv[1]
s = open(p).read()
old = re.search(r'ui_print "- Ternak TT v1\.1\.7".*?libternak_shadowhook\.so shipped via system/lib\{,64\}\."\n', s, re.DOTALL)
new = (
    'ui_print "- Ternak TT v1.2.0"\n'
    'ui_print "- + FIX v1.2.0: consume ShadowHook as AAR from Maven Central"\n'
    'ui_print "-   (com.bytedance.android:shadowhook, 2.0.1). Source build"\n'
    'ui_print "-   v1.1.7-v1.1.9 broke under NDK r26d Clang -Werror cascade"\n'
    'ui_print "-   on -Wunsafe-buffer-usage, -Wdeclaration-after-statement,"\n'
    'ui_print "-   -Wreserved-identifier. AAR sidesteps compile entirely."\n'
    'ui_print "-   SONAME rewritten via patchelf to libternak_shadowhook.so"\n'
    'ui_print "-   to avoid collision with TikTok bundled libshadowhook.so."\n'
)
if old:
    s = s.replace(old.group(0), new)
    open(p, 'w').write(s)
    print("   [patched] customize.sh header -> v1.2.0")
else:
    print("   [customize.sh] no v1.1.7 marker found, skipping (may already be v1.2.0)")
PY
    else
        echo "   [customize.sh] no v1.1.7 marker found, skipping"
    fi
fi

# 3) CHANGELOG.md prepend v1.2.0 section
if [ -f "$DST/CHANGELOG.md" ] && [ -f "$SRC/CHANGELOG-v1.2.0.md" ]; then
    if grep -q '^## v1.2.0' "$DST/CHANGELOG.md"; then
        echo "   [CHANGELOG.md] v1.2.0 section already present, skipping"
    else
        python3 - "$DST/CHANGELOG.md" "$SRC/CHANGELOG-v1.2.0.md" <<'PY'
import sys
cl_path, section_path = sys.argv[1], sys.argv[2]
cl = open(cl_path).read()
section = open(section_path).read()
# Prepend after the '---' separator that follows the header prose.
marker = '---\n\n## v'
idx = cl.find(marker)
if idx == -1:
    # Fallback: append at top.
    open(cl_path, 'w').write(section + '\n' + cl)
else:
    insert_at = idx + len('---\n\n')
    open(cl_path, 'w').write(cl[:insert_at] + section + '\n' + cl[insert_at:])
print("   [patched] CHANGELOG.md — v1.2.0 section prepended")
PY
    fi
fi

echo ""
echo "==> v1.2.0 overlay applied."
echo "    Next steps:"
echo "      git add -A"
echo "      git commit -m 'chore(release): v1.2.0 — ShadowHook AAR + patchelf SONAME'"
echo "      git push"
echo "    Then wait for GitHub Actions to build + tag v1.2.0 automatically."
