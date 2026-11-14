#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output="$(mktemp "${TMPDIR:-/tmp}/eidolon-device-settings.XXXXXX")"
trap 'rm -f "$output"' EXIT
read -r -a flags <<<"$(pkg-config --cflags --libs libcjson)"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I main tests/device_settings_core_test.cc main/eidolon/device_settings_core.cc "${flags[@]}" -o "$output"
"$output"
