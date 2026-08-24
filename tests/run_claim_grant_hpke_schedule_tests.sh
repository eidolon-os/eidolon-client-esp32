#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
output="${TMPDIR:-/tmp}/eidolon_claim_grant_hpke_schedule_tests"
read -r -a openssl_flags <<<"$(pkg-config --cflags --libs openssl)"

cd "$repo_root"
c++ -std=c++17 -Wall -Wextra -Werror \
  -Imain \
  tests/claim_grant_hpke_schedule_test.cc \
  main/eidolon/claim_grant_hpke_schedule.cc \
  "${openssl_flags[@]}" \
  -o "$output"
"$output"
