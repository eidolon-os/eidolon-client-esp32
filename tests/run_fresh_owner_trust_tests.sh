#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output="$(mktemp "${TMPDIR:-/tmp}/eidolon-fresh-trust.XXXXXX")"
trap 'rm -f "$output"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I tests/owner_trust_native/include -I tests/stubs -I main \
  tests/fresh_owner_trust_test.cc main/eidolon/hub_trust_store.cc -o "$output"
"$output"
