#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

compiler="${CXX:-c++}"
output="${TMPDIR:-/tmp}/eidolon_presence_owner_orchestration_tests"

"${compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I main/eidolon \
  tests/presence_owner_orchestration_test.cc \
  main/eidolon/ambient_presence_state.cc \
  main/eidolon/guard/owner_presence_state_machine.cc \
  -o "${output}"

"${output}"
