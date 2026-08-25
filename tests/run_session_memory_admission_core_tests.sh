#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

compiler="${CXX:-c++}"
output="${TMPDIR:-/tmp}/eidolon_session_memory_admission_core_tests"

"${compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I main -I main/eidolon \
  tests/session_memory_admission_core_test.cc \
  main/eidolon/session_memory_admission_core.cc \
  -o "${output}"

"${output}"
