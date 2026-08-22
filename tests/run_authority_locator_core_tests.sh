#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

compiler="${CXX:-c++}"
output="${TMPDIR:-/tmp}/eidolon_authority_locator_core_tests"

"${compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I main \
  tests/authority_locator_core_test.cc \
  main/eidolon/authority_locator_core.cc \
  -o "${output}"

"${output}"
