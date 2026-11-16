#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

compiler="${CXX:-c++}"
output="$(mktemp "${TMPDIR:-/tmp}/eidolon-policy-announcement.XXXXXX")"
trap 'rm -f "$output"' EXIT

"${compiler}" -std=c++17 -Wall -Wextra -Werror -fsanitize=thread \
  -I main \
  tests/policy_announcement_test.cc \
  -o "${output}"

"${output}"
