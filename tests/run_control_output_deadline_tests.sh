#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output="$(mktemp "${TMPDIR:-/tmp}/eidolon-control-deadline.XXXXXX")"
trap 'rm -f "$output"' EXIT
read -r -a flags <<<"$(pkg-config --cflags --libs libcjson)"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I tests/stubs -I main tests/control_output_deadline_test.cc main/eidolon/control_protocol.cc "${flags[@]}" -o "$output"
"$output"
