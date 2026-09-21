#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output="$(mktemp -d "${TMPDIR:-/tmp}/eidolon-pcm-capture.XXXXXX")"
trap 'rm -rf "$output"' EXIT
for box3 in 0 1; do
 "${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -DCONFIG_BOARD_TYPE_ESP_BOX_3="$box3" -I tests/pcm_capture/stubs -I tests/stubs -I main \
  -I managed_components/espressif__esp_capture/include -I managed_components/espressif__esp_capture/interface \
  tests/pcm_capture_source_test.cc main/eidolon/audio/pcm_push_capture_source.cc -o "$output/test"
 "$output/test"
done
