#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

compiler="${CXX:-c++}"
output="${TMPDIR:-/tmp}/eidolon_commissioning_transport_core_tests"

"${compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I main \
  tests/commissioning_transport_core_test.cc \
  main/eidolon/commissioning_transport_core.cc \
  -o "${output}"

"${output}"
