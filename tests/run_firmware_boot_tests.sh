#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bin="$(mktemp "${TMPDIR:-/tmp}/eidolon-firmware-boot.XXXXXX")"
trap 'rm -f "$bin"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I tests/ota_transaction/stubs -I tests/stubs -I main \
  tests/firmware_boot_test.cc -o "$bin"
"$bin"
