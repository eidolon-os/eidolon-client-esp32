#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output="$(mktemp -d "${TMPDIR:-/tmp}/eidolon-render-flush.XXXXXX")"
trap 'rm -rf "$output"' EXIT
# Compile the actual dependency function, not a second implementation. Its worker
# and event APIs are supplied by the deterministic host runtime in the test.
python3 - "$output/render_flush.inc" <<'PY'
from pathlib import Path
import sys
source = Path('components/av_render/src/av_render.c').read_text()
start = source.index('static int render_flush(')
end = source.index('\nint av_render_set_speed(', start)
Path(sys.argv[1]).write_text(source[start:end])
PY
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$output" tests/av_render_flush_test.c -o "$output/test"
"$output/test"
