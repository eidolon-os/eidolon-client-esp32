#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

compiler="${CXX:-c++}"
output="${TMPDIR:-/tmp}/eidolon_commissioning_transaction_tests"

read -r -a cjson_flags <<<"$(pkg-config --cflags --libs libcjson)"

"${compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I tests/stubs \
  -I main \
  tests/commissioning_transaction_test.cc \
  main/eidolon/commissioning_transaction.cc \
  main/eidolon/esp_idf_commissioning_credential_store.cc \
  main/eidolon/commissioning_credential.cc \
  main/eidolon/esp_idf_device_local_erase_adapter.cc \
  "${cjson_flags[@]}" \
  -o "${output}"

"${output}"
