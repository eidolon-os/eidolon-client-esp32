#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

compiler="${CXX:-c++}"
output="${TMPDIR:-/tmp}/eidolon_livekit_data_only_compat_tests"

"${compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I tests/fakes -I main \
  tests/livekit_data_only_compat_test.cc \
  -o "${output}"

"${output}"
