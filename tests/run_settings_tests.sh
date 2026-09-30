#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bin="$(mktemp "${TMPDIR:-/tmp}/eidolon-settings.XXXXXX")"
trap 'rm -f "$bin"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I tests/settings/stubs -I tests/stubs -I main \
  tests/settings_test.cc main/settings.cc -o "$bin"
"$bin"
