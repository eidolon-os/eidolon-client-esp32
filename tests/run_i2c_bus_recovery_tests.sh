#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output="$(mktemp "${TMPDIR:-/tmp}/eidolon_i2c_recovery.XXXXXX")"
trap 'rm -f "$output"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I tests/i2c_stubs -I tests/stubs -I main \
  tests/i2c_bus_recovery_test.cc main/boards/common/i2c_bus_recovery.cc -o "$output"
"$output"
