#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output="$(mktemp "${TMPDIR:-/tmp}/eidolon-ui-input.XXXXXX")"
trap 'rm -f "$output"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -I main \
 tests/ui_input_profile_test.cc main/eidolon/ui_state_mapper.cc main/eidolon/eidolon_ui_labels.cc -o "$output"
"$output"
