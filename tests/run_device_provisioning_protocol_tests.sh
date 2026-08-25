#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

compiler="${CXX:-c++}"
output="${TMPDIR:-/tmp}/eidolon_device_provisioning_protocol_tests"
read -r -a cjson_flags <<<"$(pkg-config --cflags --libs libcjson)"

"${compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I tests/stubs \
  -I main \
  tests/device_provisioning_protocol_test.cc \
  main/eidolon/device_provisioning_protocol.cc \
  main/eidolon/provisioning_window_policy_core.cc \
  "${cjson_flags[@]}" \
  -o "${output}"

"${output}"
