#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
idf="${IDF_PATH:-/Users/manson/.espressif/v5.5.4/esp-idf}"
nvs="$idf/components/nvs_flash"
output="$(mktemp -d "${TMPDIR:-/tmp}/eidolon-nvs-replay.XXXXXX")"
trap 'rm -rf "$output"' EXIT
c++ -std=c++17 -DNO_DEBUG_STORAGE -DLINUX_TARGET=1 \
 -I "$here/include" -I "$nvs/src" -I "$nvs/include" \
 -I "$nvs/private_include" -I "$idf/components/esp_common/include" \
 "$here/replay.cc" "$nvs/src/nvs_storage.cpp" "$nvs/src/nvs_page.cpp" \
 "$nvs/src/nvs_pagemanager.cpp" "$nvs/src/nvs_types.cpp" \
 "$nvs/src/nvs_item_hash_list.cpp" -lz -o "$output/replay"
for mode in baseline drop-cache extra-page slot-marker; do
 printf '%s\n' "$mode"
 "$output/replay" "${1:?Pass the private flash dump starting at 0x8000}" "$mode"
done
