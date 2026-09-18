#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
idf="${IDF_PATH:?Set IDF_PATH to ESP-IDF 5.5.4 (the BOX-3 storage implementation)}"
nvs="$idf/components/nvs_flash"
output="$(mktemp -d "${TMPDIR:-/tmp}/eidolon-nvs-integration.XXXXXX")"
trap 'rm -rf "$output"' EXIT
read -r -a libs <<<"$(pkg-config --cflags --libs libcjson openssl)"
"${CXX:-c++}" -std=c++17 -DNO_DEBUG_STORAGE -DLINUX_TARGET=1 \
 -I tests/nvs_native/include -I "$nvs/include" -I "$idf/components/esp_common/include" \
 -I "$nvs/src" -I "$nvs/private_include" -I main -I tests/stubs \
 tests/nvs_native/commissioning_replay.cc \
 main/eidolon/commissioning_transaction.cc main/eidolon/esp_idf_commissioning_credential_store.cc \
 main/eidolon/commissioning_credential.cc main/eidolon/esp_idf_device_local_erase_adapter.cc \
 "$nvs/src/nvs_storage.cpp" "$nvs/src/nvs_page.cpp" "$nvs/src/nvs_pagemanager.cpp" \
 "$nvs/src/nvs_types.cpp" "$nvs/src/nvs_item_hash_list.cpp" \
 "${libs[@]}" -lz -o "$output/replay"
"$output/replay" "$@"
