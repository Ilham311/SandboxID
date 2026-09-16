#!/usr/bin/env bash
#
# Host-level verification for the identity-spoofing logic.
#
# These are not unit tests of the Zygisk module (that needs a device and a
# Zygisk provider). They pin down the *deterministic core* the module and the
# CLI both build on, and — uniquely among the tests in this repo — they execute
# the real per-fd synth state machine from jni/module_hooks.cpp by including
# that translation unit, so the hook code under test is production code, not a
# copy of it.
#
# Three suites:
#   sbx_pure       pure functions: classify(), MAC validity, AppLog determinism,
#                  patch_applog_xml/patch_meminfo/patch_cpuinfo, uuid/snowflake.
#   sbx_hook_sm    the per-fd state machine against this host's REAL /proc files:
#                  open/read/EOF, lseek rewind, chunked reads, pread64, close,
#                  relative openat, and fail-closed behaviour.
#   console_check  the WebUI console module (webroot/console.js) under a stubbed
#                  DOM: capture, badge semantics, stream guards, save transport.
#                  Skipped unless node is on PATH.
#
# Requirements: a C++20 compiler (clang++ or g++) on the host, a JDK for jni.h,
# and node for the console suite. No NDK needed — the two Android-only headers
# the module pulls in are shimmed under tests/host/include when the host lacks
# them. Both lookups are probes: a host that already has the real headers
# (Termux, or a box with NDK headers on the path) uses them untouched.
# Usage:   ./tests/host/run.sh          # build + run all suites
#          CXX=g++ ./tests/host/run.sh

set -eu

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CXX="${CXX:-clang++}"

if ! command -v "$CXX" >/dev/null 2>&1; then
    echo "run.sh: C++ compiler '$CXX' not found (set CXX= to override)" >&2
    exit 2
fi

# jni/zygisk.hpp is .gitignored — build.sh downloads it from a pinned commit
# and checksums it. The suites compile the real module headers, which include
# it, so a fresh checkout cannot run these tests until it is present. Fetch it
# the same way rather than leaving that to the build job. The pin is parsed out
# of build.sh so there is one source of truth; a second copy here would drift.
if [ ! -f "$REPO/jni/zygisk.hpp" ]; then
    z_commit=$(sed -n 's/^ZYGISK_HPP_COMMIT="\{0,1\}\([^"]*\)"\{0,1\}$/\1/p' "$REPO/build.sh" | head -n 1)
    if [ -z "$z_commit" ]; then
        echo "run.sh: could not read the zygisk.hpp pin from build.sh" >&2
        exit 2
    fi
    echo "run.sh: jni/zygisk.hpp absent (build.sh fetches it), pulling @ $z_commit"
    if ! curl -fsSL -o "$REPO/jni/zygisk.hpp" \
        "https://raw.githubusercontent.com/topjohnwu/zygisk-module-sample/$z_commit/module/jni/zygisk.hpp"; then
        echo "run.sh: fetch failed — check network, or place jni/zygisk.hpp by hand" >&2
        exit 2
    fi
fi
# Verify it whether it was just fetched or was already there: the pin is
# supply-chain protection, and silently compiling against a different
# zygisk.hpp than the module ships would test the wrong thing.
z_sha=$(sed -n 's/^ZYGISK_HPP_SHA256="\{0,1\}\([^"]*\)"\{0,1\}$/\1/p' "$REPO/build.sh" | head -n 1)
if command -v sha256sum >/dev/null 2>&1; then
    z_got=$(sha256sum "$REPO/jni/zygisk.hpp" | cut -d' ' -f1)
elif command -v shasum >/dev/null 2>&1; then
    z_got=$(shasum -a 256 "$REPO/jni/zygisk.hpp" | cut -d' ' -f1)
else
    echo "run.sh: no sha256 tool (sha256sum/shasum) to verify zygisk.hpp" >&2
    exit 2
fi
if [ "$z_got" != "$z_sha" ]; then
    echo "run.sh: zygisk.hpp checksum mismatch — expected $z_sha, got $z_got" >&2
    echo "         delete jni/zygisk.hpp to re-fetch from the pinned commit" >&2
    exit 2
fi

# -DNDEBUG keeps release behaviour: LOGD compiles to nothing, exactly as the
# shipped release variant does, so the test exercises what users actually run.
# Declared before the jni.h probe below, which APPENDS to FLAGS — an array
# assignment here would reset it and silently discard the probe's -I flags.
FLAGS=(-std=c++20 -O1 -DNDEBUG -Wall -Wextra
       -I"$REPO/jni" -I"$REPO/native/include")

# The TUs under test include the real module headers, which pull in <jni.h>.
# Some hosts (notably GitHub's ubuntu runners) do not ship it on the default
# path, and the failure then surfaces as an opaque 'fatal error: jni.h file not
# found'. Locate a JDK include dir instead of leaving that to chance.
if ! echo '#include <jni.h>' | "$CXX" -fsyntax-only -x c++ - >/dev/null 2>&1; then
    jni_inc=$(ls -d /usr/lib/jvm/*/include 2>/dev/null | head -n 1)
    if [ -n "$jni_inc" ]; then
        echo "run.sh: jni.h not on the default path, using $jni_inc"
        # jni.h does `#include "jni_md.h"`, which on a Linux JDK sits one level
        # deeper in include/linux. A quoted include searches the including
        # file's own directory first and then the -I list, so include/ alone
        # would fail with 'jni_md.h file not found' — both dirs are needed.
        FLAGS+=("-I$jni_inc" "-I$jni_inc/linux")
    else
        echo "run.sh: jni.h not found — install a JDK (e.g. 'sudo apt install default-jdk')" >&2
        exit 2
    fi
fi

# The module headers also include two Android-only headers: <android/log.h>
# and <sys/system_properties.h>. Termux ships both; a stock Linux runner ships
# neither, and the suite would die at the #include before any check ran.
# tests/host/include holds minimal shims for the tiny surface actually used
# (see those files for why a declaration/incomplete-type shim is faithful).
# Added only when the real headers are absent, so a machine that has them
# keeps compiling against the real ones.
if ! { echo '#include <android/log.h>'; echo '#include <sys/system_properties.h>'; } \
     | "$CXX" -fsyntax-only -x c++ - >/dev/null 2>&1; then
    echo "run.sh: android headers not on the default path, using tests/host/include shims"
    FLAGS+=("-I$REPO/tests/host/include")
fi

OUT="${TMPDIR:-/tmp}/sbx-host-tests"
mkdir -p "$OUT"

exit_code=0

for suite in sbx_pure sbx_hook_sm; do
    src="$REPO/tests/host/$suite.cpp"
    bin="$OUT/$suite"

    echo "==> building $suite"
    if ! "$CXX" "${FLAGS[@]}" "$src" "$REPO/native/native_read.cpp" -o "$bin"; then
        echo "FAIL: $suite did not compile"
        exit_code=1
        continue
    fi

    echo "==> running $suite"
    if ! "$bin"; then
        echo "FAIL: $suite reported failures"
        exit_code=1
    fi
done

# ---- console_check: webroot/console.js under a stubbed DOM -----------------
# node is not a build requirement of this repo, so absent node is a skip, not
# a failure — the C++ suites above are the ones that must always run.
if command -v node >/dev/null 2>&1; then
    echo "==> running console_check"
    if ! node "$REPO/tests/host/console_check.cjs"; then
        echo "FAIL: console_check reported failures"
        exit_code=1
    fi
else
    echo "==> skipping console_check (node not found)"
fi

if [ "$exit_code" -eq 0 ]; then
    echo "==> all host suites passed"
else
    echo "==> host suites FAILED"
fi
exit "$exit_code"
