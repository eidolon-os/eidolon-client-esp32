#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
output="${TMPDIR:-/tmp}/eidolon_http_date_utc_tests"

cd "$repo_root"
c++ -std=c++17 -Wall -Wextra -Werror \
  -Imain \
  tests/http_date_utc_test.cc \
  main/eidolon/http_date_utc.cc \
  main/eidolon/rfc3339_utc.cc \
  -o "$output"
"$output"
