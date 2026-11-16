#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output="$(mktemp "${TMPDIR:-/tmp}/eidolon-pet-runtime.XXXXXX")"
trap 'rm -f "$output"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I main tests/pet_runtime_test.cc main/eidolon/pet/pet_runtime.cc -o "$output"
"$output"
