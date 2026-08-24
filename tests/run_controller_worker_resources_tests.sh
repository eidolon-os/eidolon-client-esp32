#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

compiler="${CXX:-c++}"
output="${TMPDIR:-/tmp}/eidolon_controller_worker_resources_tests"

"${compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I main \
  tests/controller_worker_resources_test.cc \
  -o "${output}"

"${output}"
echo "controller_worker_resources_test: PASS"
