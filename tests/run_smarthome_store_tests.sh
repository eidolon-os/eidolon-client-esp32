#!/usr/bin/env bash
# Smart home panel core (main/eidolon/smarthome) against the SDK goldens in
# ../eidolon_sdk/contracts/smarthome/v1/golden. Run from anywhere.
set -euo pipefail

cd "$(dirname "$0")/.."

output="$(mktemp "${TMPDIR:-/tmp}/eidolon-smarthome-store.XXXXXX")"
trap 'rm -f "$output"' EXIT
read -r -a cjson_flags <<<"$(pkg-config --cflags --libs libcjson)"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I tests/stubs \
  -I main \
  tests/smarthome_store_test.cc \
  main/eidolon/control_protocol.cc \
  main/eidolon/smarthome/smarthome_wire.cc \
  main/eidolon/smarthome/smarthome_store.cc \
  main/eidolon/smarthome/smarthome_tiles.cc \
  "${cjson_flags[@]}" \
  -o "${output}"

"${output}"
