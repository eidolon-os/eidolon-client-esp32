#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
compiler="${CXX:-c++}"
output="${TMPDIR:-/tmp}/eidolon_device_instance_identity_tests"

"${compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I main \
  tests/device_instance_identity_test.cc \
  main/eidolon/device_instance_identity.cc \
  -o "${output}"
"${output}"
