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
# and node for the console suite. No NDK needed.
# Usage:   ./tests/host/run.sh          # build + run all suites
#          CXX=g++ ./tests/host/run.sh

set -eu

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CXX="${CXX:-clang++}"

if ! command -v "$CXX" >/dev/null 2>&1; then
    echo "run.sh: C++ compiler '$CXX' not found (set CXX= to override)" >&2
    exit 2
fi

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

OUT="${TMPDIR:-/tmp}/sbx-host-tests"
mkdir -p "$OUT"

# -DNDEBUG keeps release behaviour: LOGD compiles to nothing, exactly as the
# shipped release variant does, so the test exercises what users actually run.
FLAGS=(-std=c++20 -O1 -DNDEBUG -Wall -Wextra
       -I"$REPO/jni" -I"$REPO/native/include")

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
