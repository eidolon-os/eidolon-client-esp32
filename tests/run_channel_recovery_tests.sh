#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

compiler="${CXX:-c++}"
output="${TMPDIR:-/tmp}/eidolon_channel_recovery_tests"

"${compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I main -I main/eidolon \
  tests/channel_recovery_test.cc \
  -o "${output}"

"${output}"
