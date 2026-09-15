#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

compiler="${CXX:-c++}"
output="${TMPDIR:-/tmp}/eidolon_companion_manifest_tests"
read -r -a cjson_flags <<<"$(pkg-config --cflags --libs libcjson)"

"${compiler}" -DCONFIG_EIDOLON_COMPANION_FACE=1 -std=c++17 -Wall -Wextra -Werror \
  -I tests/stubs \
  -I main \
  tests/companion_manifest_test.cc \
  main/eidolon/hub_onboarding_protocol.cc \
  main/eidolon/output_policy.cc \
  main/eidolon/hub_txt_parser.cc \
  main/eidolon/device_manifest_assertion_core.cc \
  "${cjson_flags[@]}" \
  -o "${output}"

"${output}"
