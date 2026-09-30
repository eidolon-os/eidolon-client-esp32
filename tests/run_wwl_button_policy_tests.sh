#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

compiler="${CXX:-c++}"
output="${TMPDIR:-/tmp}/eidolon_wwl_button_policy_tests"

"${compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I main \
  -I "main/boards/wwl-ball-s3-lcd-1.85" \
  tests/wwl_button_policy_test.cc \
  "main/boards/wwl-ball-s3-lcd-1.85/wwl_button_policy.cc" \
  main/eidolon/provisioning_window_policy_core.cc \
  -o "${output}"

"${output}"