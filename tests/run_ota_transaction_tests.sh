#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
bin="$(mktemp -t eidolon-ota-test)"
trap 'rm -f "$bin"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$root/tests/ota_transaction/stubs" -I"$root/main" \
  "$root/tests/ota_transaction/test.cc" -o "$bin"
"$bin"
