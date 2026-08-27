#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

compiler="${CXX:-c++}"
output="${TMPDIR:-/tmp}/eidolon_host_resolution_core_tests"

"${compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I main \
  tests/host_resolution_core_test.cc \
  main/eidolon/host_resolution_core.cc \
  -o "${output}"

"${output}"
