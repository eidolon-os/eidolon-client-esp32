#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
output="${TMPDIR:-/tmp}/eidolon_device_local_erase_adapter_tests"

cd "$repo_root"
c++ -std=c++20 -Wall -Wextra -Werror \
  -Imain \
  tests/device_local_erase_adapter_test.cc \
  main/eidolon/esp_idf_device_local_erase_adapter.cc \
  -o "$output"
"$output"
