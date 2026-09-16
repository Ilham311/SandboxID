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
# Two suites:
#   sbx_pure     pure functions: classify(), MAC validity, AppLog determinism,
#                patch_applog_xml/patch_meminfo/patch_cpuinfo, uuid/snowflake.
#   sbx_hook_sm  the per-fd state machine against this host's REAL /proc files:
#                open/read/EOF, lseek rewind, chunked reads, pread64, close,
#                relative openat, and fail-closed behaviour.
#
# Requirements: a C++20 compiler (clang++ or g++) on the host. No NDK needed.
# Usage:   ./tests/host/run.sh          # build + run both suites
#          CXX=g++ ./tests/host/run.sh

set -eu

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CXX="${CXX:-clang++}"

if ! command -v "$CXX" >/dev/null 2>&1; then
    echo "run.sh: C++ compiler '$CXX' not found (set CXX= to override)" >&2
    exit 2
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

if [ "$exit_code" -eq 0 ]; then
    echo "==> all host suites passed"
else
    echo "==> host suites FAILED"
fi
exit "$exit_code"
