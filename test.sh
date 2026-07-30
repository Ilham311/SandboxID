#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

echo "==> Compiling test_proc_utils.cpp"
g++ -std=c++11 -Ijni jni/test_proc_utils.cpp -o test_proc_utils

echo "==> Running tests"
./test_proc_utils

echo "==> Cleaning up"
rm test_proc_utils
