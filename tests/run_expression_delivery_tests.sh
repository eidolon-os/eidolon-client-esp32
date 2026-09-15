#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output="$(mktemp "${TMPDIR:-/tmp}/eidolon-delivery.XXXXXX")"
trap 'rm -f "$output"' EXIT
read -r -a flags <<<"$(pkg-config --cflags --libs libcjson)"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I main tests/expression_delivery_test.cc main/eidolon/expression/delivery.cc main/eidolon/expression/core/runtime.cc main/eidolon/expression/core/wire.cc "${flags[@]}" -o "$output"
"$output"
