#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output="${TMPDIR:-/tmp}/eidolon_shared_transport_lease_tests"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I main tests/shared_transport_lease_test.cc -o "$output"
"$output"
