#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

compiler="${CXX:-c++}"
output="${TMPDIR:-/tmp}/eidolon_ui_labels_tests"

"${compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I main \
  tests/eidolon_ui_labels_test.cc \
  main/eidolon/eidolon_ui_labels.cc \
  main/eidolon/ui_state_mapper.cc \
  -o "${output}"

"${output}"
