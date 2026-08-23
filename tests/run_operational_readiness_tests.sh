#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

compiler="${CXX:-c++}"
output="${TMPDIR:-/tmp}/eidolon_operational_readiness_tests"

"${compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I main \
  tests/operational_readiness_test.cc \
  -o "${output}"

"${output}"
