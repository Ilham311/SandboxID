#!/usr/bin/env bash
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
HOST_CXX="${CXX:-c++}"

echo "Compiling prop_rules_test..."
$HOST_CXX -std=c++20 -DTT_HOST_TEST $DIR/prop_rules_test.cpp -o $DIR/prop_rules_test

echo "Running prop_rules_test..."
$DIR/prop_rules_test

echo ""
echo "Running persona validator test script..."
bash $DIR/persona_validator_test.sh

echo "Cleaning up..."
rm -f $DIR/prop_rules_test
rm -f $DIR/ternak-tt-host

echo "All tests completed successfully."
