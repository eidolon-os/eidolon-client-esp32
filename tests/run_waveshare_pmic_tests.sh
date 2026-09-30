#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output="$(mktemp "${TMPDIR:-/tmp}/eidolon_pmic.XXXXXX")"
trap 'rm -f "$output"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I tests/i2c_stubs -I tests/stubs -I main tests/waveshare_pmic_test.cc -o "$output"
"$output"
