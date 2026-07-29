#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "${build_dir}"' EXIT

c++ -std=c++20 -Wall -Wextra -Werror \
  -I"${repo_root}/main/eidolon" \
  "${repo_root}/tests/ambient_presence_state_test.cc" \
  "${repo_root}/main/eidolon/ambient_presence_state.cc" \
  -o "${build_dir}/ambient_presence_state_test"

"${build_dir}/ambient_presence_state_test"
