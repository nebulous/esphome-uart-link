#!/usr/bin/env bash
# Compile and run the SPSCRingBuffer unit tests with the host toolchain.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/uart-link-test-spsc"

c++ -std=c++17 -Wall -Wextra -O1 "$root/unit/test_spsc.cpp" -o "$out"
"$out"
