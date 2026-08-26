#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

compiler="${CXX:-c++}"
output="${TMPDIR:-/tmp}/eidolon_commissioning_transaction_tests"

"${compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I tests/stubs \
  -I main \
  tests/commissioning_transaction_test.cc \
  main/eidolon/commissioning_transaction.cc \
  -o "${output}"

"${output}"
