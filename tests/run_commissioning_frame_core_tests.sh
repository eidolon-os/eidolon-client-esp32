#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
output="${TMPDIR:-/tmp}/eidolon_commissioning_frame_core_tests"
read -r -a cjson_flags <<<"$(pkg-config --cflags --libs libcjson)"

cd "$repo_root"
c++ -std=c++17 -Wall -Wextra -Werror \
  -Imain \
  tests/commissioning_frame_core_test.cc \
  main/eidolon/commissioning_frame_core.cc \
  "${cjson_flags[@]}" \
  -o "$output"
"$output"
echo "commissioning frame core: PASS"
