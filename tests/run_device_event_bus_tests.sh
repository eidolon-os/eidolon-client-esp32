#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

c_compiler="${CC:-cc}"
cxx_compiler="${CXX:-c++}"
test_dir="${TMPDIR:-/tmp}/eidolon_device_event_bus_tests"
cjson_dir="$(pwd)/managed_components/espressif__cjson/cJSON"
[[ -f "${cjson_dir}/cJSON.c" && -f "${cjson_dir}/cJSON.h" ]] || {
  echo "error: the project-pinned cJSON component is unavailable" >&2
  exit 1
}
mkdir -p "${test_dir}"

"${c_compiler}" -std=c11 -Wall -Wextra -Werror \
  -I "${cjson_dir}" \
  -c "${cjson_dir}/cJSON.c" \
  -o "${test_dir}/cJSON.o"

"${cxx_compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I main/eidolon \
  -I "${cjson_dir}" \
  tests/device_event_bus_test.cc \
  main/eidolon/device_event_builder.cc \
  main/eidolon/device_event_bus.cc \
  "${test_dir}/cJSON.o" \
  -o "${test_dir}/device_event_bus_tests"

"${test_dir}/device_event_bus_tests"
