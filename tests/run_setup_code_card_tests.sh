#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
output="${TMPDIR:-/tmp}/eidolon_setup_code_card_tests"
read -r -a cjson_flags <<<"$(pkg-config --cflags --libs libcjson)"

cd "$repo_root"
c++ -std=c++17 -Wall -Wextra -Werror \
  -Imain \
  tests/setup_code_card_test.cc \
  main/eidolon/setup_code_card.cc \
  main/eidolon/setup_code_secret.cc \
  "${cjson_flags[@]}" \
  -o "$output"
"$output"
echo "setup code card: PASS"
