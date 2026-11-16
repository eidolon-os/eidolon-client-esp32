#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output="${EIDOLON_UI_TEST_BUILD:-${TMPDIR:-/tmp}/eidolon-pet-panel-ui-tests}"
cmake -S tests/pet_panel_ui -B "$output" -DCMAKE_BUILD_TYPE=Release
cmake --build "$output" -j 8
"$output/pet_panel_ui_tests"
