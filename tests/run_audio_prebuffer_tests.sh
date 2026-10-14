#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output="$(mktemp -d "${TMPDIR:-/tmp}/eidolon-prebuffer.XXXXXX")"
trap 'rm -rf "$output"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I components/av_render/src tests/audio_prebuffer_test.c -o "$output/test"
"$output/test"
