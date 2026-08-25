#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

compiler="${CXX:-c++}"
output="${TMPDIR:-/tmp}/eidolon_provisioning_window_policy_core_tests"

"${compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I main \
  tests/provisioning_window_policy_core_test.cc \
  main/eidolon/provisioning_window_policy_core.cc \
  -o "${output}"

"${output}"
