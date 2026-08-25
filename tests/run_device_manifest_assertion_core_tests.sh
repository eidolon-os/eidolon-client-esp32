#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
output="${TMPDIR:-/tmp}/eidolon_device_manifest_assertion_core_tests"

cd "$repo_root"
c++ -std=c++17 -Wall -Wextra -Werror \
  -Imain \
  tests/device_manifest_assertion_core_test.cc \
  main/eidolon/device_manifest_assertion_core.cc \
  -o "$output"
"$output"
