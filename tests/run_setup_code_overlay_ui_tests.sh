#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output="${EIDOLON_UI_TEST_BUILD:-${TMPDIR:-/tmp}/eidolon-setup-code-overlay-ui-tests}"
cmake -S tests/setup_code_overlay_ui -B "$output" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$output" -j 8 >/dev/null
mkdir -p "$output/screenshots"
"$output/setup_code_overlay_ui_tests" "$output/screenshots"
echo "setup code overlay: PASS ($output/screenshots)"
