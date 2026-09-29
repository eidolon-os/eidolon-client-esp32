#!/usr/bin/env bash
# korvo-1 smart home panel (HomePanelView) render and interaction checks; writes
# one PPM per state to the build directory's screenshots folder.
set -euo pipefail
cd "$(dirname "$0")/.."
output="${EIDOLON_UI_TEST_BUILD:-${TMPDIR:-/tmp}/eidolon-home-panel-ui-tests}"
cmake -S tests/home_panel_ui -B "$output" -DCMAKE_BUILD_TYPE=Release
cmake --build "$output" -j 8
mkdir -p "$output/screenshots"
"$output/home_panel_ui_tests" "$output/screenshots"
