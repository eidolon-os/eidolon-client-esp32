#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

compiler="${CXX:-c++}"
output="${TMPDIR:-/tmp}/eidolon_companion_manifest_tests"
read -r -a cjson_flags <<<"$(pkg-config --cflags --libs libcjson)"

for face in 0 1; do
"${compiler}" -DCONFIG_EIDOLON_COMPANION_FACE="${face}" -DCONFIG_EIDOLON_CAP_EXPRESSION=1 -DCONFIG_EIDOLON_CAP_DIALOGUE_TEXT=1 -std=c++17 -Wall -Wextra -Werror \
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
done
