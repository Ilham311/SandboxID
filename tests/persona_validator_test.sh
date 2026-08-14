#!/usr/bin/env sh
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="$DIR/../jni/ternak-tt"

# We must compile a host version of ternak-tt for testing validation
echo "Building host ternak-tt for validation..."
# Actually, wait, ternak-tt is built for Android via NDK. To test `validate`,
# we can compile a minimal host version just of ternak-tt.cpp if we stub out pool_tt.hpp dependencies
# OR we can just write a small parser in prop_rules_test.cpp and include the validation logic there.
# Let's compile ternak-tt.cpp directly for host to run the validate command.

HOST_CXX="${CXX:-c++}"

echo "Compiling host validation tool..."
$HOST_CXX -std=c++20 -DTT_HOST_TEST $DIR/../jni/ternak-tt.cpp -o $DIR/ternak-tt-host

echo "Testing good persona..."
if ! $DIR/ternak-tt-host validate $DIR/fixtures/identity.good.prop; then
    echo "ERROR: good persona failed validation"
    exit 1
fi

echo "Testing bad persona..."
if $DIR/ternak-tt-host validate $DIR/fixtures/identity.bad.prop; then
    echo "ERROR: bad persona passed validation (it should fail)"
    exit 1
fi

echo "Testing bad vndk persona..."
if $DIR/ternak-tt-host validate $DIR/fixtures/identity.bad_vndk.prop; then
    echo "ERROR: bad vndk persona passed validation (it should fail)"
    exit 1
fi

echo "All persona validation tests passed."
