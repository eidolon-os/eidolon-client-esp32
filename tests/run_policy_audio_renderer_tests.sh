#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output="$(mktemp -d "${TMPDIR:-/tmp}/eidolon-render-policy.XXXXXX")"
trap 'rm -rf "$output"' EXIT
render=managed_components/tempotian__av_render
sal=managed_components/espressif__media_lib_sal/include
read -r -a flags <<<"$(pkg-config --cflags --libs libcjson)"
"${CC:-cc}" -fsanitize=address,undefined -I tests/stubs -I "$render/include" -I "$sal" -I "$sal/port" -c "$render/src/audio_render.c" -o "$output/render.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I main -I tests/stubs -I "$render/include" -I "$sal" -I "$sal/port" tests/policy_audio_renderer_test.cc \
  main/eidolon/policy_audio_renderer.cc main/eidolon/output_policy.cc "$output/render.o" "${flags[@]}" -o "$output/test"
"$output/test"
