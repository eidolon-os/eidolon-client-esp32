#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "${build_dir}"' EXIT

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -I"${repo_root}/main/eidolon" \
  "${repo_root}/tests/presence_wake_flow_test.cc" \
  "${repo_root}/main/eidolon/presence_wake_flow.cc" \
  -o "${build_dir}/presence_wake_flow_test"

"${build_dir}/presence_wake_flow_test"
