#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

compiler="${CXX:-c++}"
output="${TMPDIR:-/tmp}/eidolon_owner_trust_storage_policy_tests"

"${compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I main \
  tests/owner_trust_storage_policy_test.cc \
  -o "${output}"
"${output}"

grep -Eq '^owner_trust,[[:space:]]*data,[[:space:]]*nvs,[[:space:]]*0x10000,[[:space:]]*0x10000,' \
  partitions/v2/16m_eidolon_box3.csv

echo "owner_trust_storage_policy_test: PASS"
