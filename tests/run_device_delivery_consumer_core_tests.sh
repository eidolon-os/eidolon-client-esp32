#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
output="${TMPDIR:-/tmp}/eidolon_device_delivery_consumer_core_tests"
read -r -a cjson_flags <<<"$(pkg-config --cflags --libs libcjson)"

cd "$repo_root"
c++ -std=c++20 -Wall -Wextra -Werror \
  -Imain \
  tests/device_delivery_consumer_core_test.cc \
  main/eidolon/device_delivery_consumer_core.cc \
  main/eidolon/device_local_erase_core.cc \
  "${cjson_flags[@]}" \
  -o "$output"
"$output"
