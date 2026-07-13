#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

compiler="${CXX:-c++}"
output="${TMPDIR:-/tmp}/eidolon_guard_runtime_tests"

"${compiler}" -std=c++17 -Wall -Wextra -Werror -Wno-unused-parameter \
  -DCONFIG_EIDOLON_GUARD_SERVICE=1 \
  -I main -I main/eidolon \
  tests/guard_runtime_state_test.cc \
  main/eidolon/guard/guard_motion.cc \
  main/eidolon/guard/guard_presence_adapter.cc \
  main/eidolon/guard/guard_state_machine.cc \
  -o "${output}"

"${output}"
