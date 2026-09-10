#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output="${TMPDIR:-/tmp}/eidolon_hub_discovery_tests"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -I tests/stubs/discovery -I tests/stubs -I main \
  tests/hub_discovery_test.cc main/eidolon/hub_discovery.cc \
  main/eidolon/hub_txt_parser.cc -o "${output}"
"${output}"
