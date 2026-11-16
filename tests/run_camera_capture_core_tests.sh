#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output="$(mktemp "${TMPDIR:-/tmp}/eidolon-camera-capture.XXXXXX")"
trap 'rm -f "$output"' EXIT
read -r -a flags <<<"$(pkg-config --cflags --libs libcjson)"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I main tests/camera_capture_core_test.cc main/eidolon/camera_capture_core.cc \
  main/eidolon/capability_rpc_core.cc "${flags[@]}" -o "$output"
"$output"
