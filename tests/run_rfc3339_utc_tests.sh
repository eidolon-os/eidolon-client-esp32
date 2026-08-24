#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
output="${TMPDIR:-/tmp}/eidolon_rfc3339_utc_tests"

cd "$repo_root"
c++ -std=c++17 -Wall -Wextra -Werror \
  -Imain \
  tests/rfc3339_utc_test.cc \
  main/eidolon/rfc3339_utc.cc \
  -o "$output"
"$output"
