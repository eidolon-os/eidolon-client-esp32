#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
output="${TMPDIR:-/tmp}/eidolon_device_control_proof_documents_tests"
read -r -a cjson_flags <<<"$(pkg-config --cflags --libs libcjson)"

cd "$repo_root"
c++ -std=c++17 -Wall -Wextra -Werror \
  -Imain \
  tests/device_control_proof_documents_test.cc \
  main/eidolon/device_control_proof_documents.cc \
  main/eidolon/device_claim_consumer_core.cc \
  "${cjson_flags[@]}" \
  -o "$output"
"$output"
