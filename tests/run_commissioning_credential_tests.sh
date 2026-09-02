#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

: "${IDF_PATH:?IDF_PATH is required; source the ESP-IDF export.sh first}"

c_compiler="${CC:-cc}"
cxx_compiler="${CXX:-c++}"
test_dir="${TMPDIR:-/tmp}/eidolon_commissioning_credential_tests"
mkdir -p "${test_dir}"

"${c_compiler}" -std=c11 -Wall -Wextra -Werror \
  -I "${IDF_PATH}/components/json/cJSON" \
  -c "${IDF_PATH}/components/json/cJSON/cJSON.c" \
  -o "${test_dir}/cJSON.o"

"${cxx_compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I main/eidolon \
  -I "${IDF_PATH}/components/json/cJSON" \
  tests/commissioning_credential_test.cc \
  main/eidolon/commissioning_credential.cc \
  "${test_dir}/cJSON.o" \
  -o "${test_dir}/commissioning_credential_tests"

"${test_dir}/commissioning_credential_tests"
