#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
out="$(mktemp -d "${TMPDIR:-/tmp}/box3-audio-schedule.XXXXXX")"
trap 'rm -rf "$out"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -I main tests/box3_audio_schedule_test.cc -o "$out/test"
"$out/test"
