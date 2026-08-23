#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

compiler="${CXX:-c++}"
output="${TMPDIR:-/tmp}/eidolon_activation_worker_resources_tests"

"${compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I tests/stubs \
  -I main \
  tests/activation_worker_resources_test.cc \
  -o "${output}"

"${output}"
echo "activation_worker_resources_test: PASS"
