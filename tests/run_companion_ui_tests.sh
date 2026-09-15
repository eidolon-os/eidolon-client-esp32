#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output="${EIDOLON_UI_TEST_BUILD:-${TMPDIR:-/tmp}/eidolon-companion-ui-tests}"
cmake -S tests/companion_ui -B "$output" -DCMAKE_BUILD_TYPE=Release
cmake --build "$output" -j 8
mkdir -p "$output/screenshots"
"$output/companion_ui_tests" "$output/screenshots"
