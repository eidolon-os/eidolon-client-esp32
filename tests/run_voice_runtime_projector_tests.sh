#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

compiler="${CXX:-c++}"
output="${TMPDIR:-/tmp}/eidolon_voice_runtime_projector_tests"

"${compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I main \
  tests/voice_runtime_projector_test.cc \
  main/eidolon/voice_runtime_projector.cc \
  -o "${output}"

"${output}"
