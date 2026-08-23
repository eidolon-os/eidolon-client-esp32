#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
output="${TMPDIR:-/tmp}/eidolon_device_local_erase_core_tests"

cd "$repo_root"
c++ -std=c++20 -Wall -Wextra -Werror \
  -Imain \
  tests/device_local_erase_core_test.cc \
  main/eidolon/device_local_erase_core.cc \
  -o "$output"
"$output"
