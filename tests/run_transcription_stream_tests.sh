#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output="$(mktemp "${TMPDIR:-/tmp}/eidolon-transcription-stream.XXXXXX")"
trap 'rm -f "$output"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I main tests/transcription_stream_test.cc -o "$output"
"$output"
