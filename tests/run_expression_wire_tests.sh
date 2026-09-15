#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output="$(mktemp -d "${TMPDIR:-/tmp}/eidolon-expression-wire.XXXXXX")"
trap 'rm -rf "$output"' EXIT
cjson=managed_components/espressif__cjson/cJSON
"${CC:-cc}" -fsanitize=address,undefined -I "$cjson" -c "$cjson/cJSON.c" -o "$output/cjson.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
 -I main -I "$cjson" tests/expression_wire_test.cc main/eidolon/expression/core/wire.cc \
 main/eidolon/expression/core/runtime.cc "$output/cjson.o" -o "$output/test"
"$output/test"
