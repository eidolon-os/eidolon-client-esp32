#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

compiler="${CXX:-c++}"
output="${TMPDIR:-/tmp}/eidolon_hub_onboarding_protocol_tests"
read -r -a cjson_flags <<<"$(pkg-config --cflags --libs libcjson)"

"${compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I tests/stubs \
  -I main \
  tests/hub_onboarding_protocol_test.cc \
  main/eidolon/hub_onboarding_protocol.cc \
  main/eidolon/hub_txt_parser.cc \
  main/eidolon/device_manifest_assertion_core.cc \
  "${cjson_flags[@]}" \
  -o "${output}"

"${output}"
