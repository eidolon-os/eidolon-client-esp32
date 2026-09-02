#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

compiler="${CXX:-c++}"
output="${TMPDIR:-/tmp}/eidolon_owner_trust_commissioner_tests"
read -r -a cjson_flags <<<"$(pkg-config --cflags --libs libcjson)"

"${compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I tests/stubs \
  -I main \
  tests/owner_trust_commissioner_test.cc \
  main/eidolon/owner_trust_commissioner.cc \
  main/eidolon/commissioning_credential.cc \
  main/eidolon/device_provisioning_protocol.cc \
  main/eidolon/hub_onboarding_protocol.cc \
  main/eidolon/hub_txt_parser.cc \
  "${cjson_flags[@]}" \
  -o "${output}"

"${output}"
