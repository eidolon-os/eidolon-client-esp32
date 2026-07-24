#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

compiler="${CXX:-c++}"
output="${TMPDIR:-/tmp}/radar_presence_tracker_tests"

"${compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I main/boards/esp-box-3 \
  tests/radar_presence_tracker_test.cc \
  main/boards/esp-box-3/radar_presence_tracker.cc \
  -o "${output}"

"${output}"
