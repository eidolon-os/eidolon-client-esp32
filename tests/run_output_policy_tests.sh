#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output="$(mktemp "${TMPDIR:-/tmp}/eidolon-output-policy.XXXXXX")"
trap 'rm -f "$output"' EXIT
read -r -a flags <<<"$(pkg-config --cflags --libs libcjson)"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I main tests/output_policy_test.cc main/eidolon/output_policy.cc "${flags[@]}" -o "$output"
"$output"
