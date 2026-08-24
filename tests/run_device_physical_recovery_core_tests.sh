#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
compiler="${CXX:-c++}"
output="${TMPDIR:-/tmp}/eidolon_device_physical_recovery_core_tests"

"${compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I main \
  tests/device_physical_recovery_core_test.cc \
  main/eidolon/device_physical_recovery_core.cc \
  -o "${output}"
"${output}"
