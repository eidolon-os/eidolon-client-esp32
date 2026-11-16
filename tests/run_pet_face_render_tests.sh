#!/usr/bin/env bash
# Host render of the desk-pet face; screenshots land in $1 (default: a temp dir).
set -euo pipefail
cd "$(dirname "$0")/.."
build="$(mktemp -d "${TMPDIR:-/tmp}/eidolon-pet-face.XXXXXX")"
out="${1:-$build/screenshots}"
mkdir -p "$out"
cmake -S tests/pet_face_render -B "$build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$build" -j 8 >/dev/null
"$build/pet_face_render_tests" "$out"
echo "screenshots: $out"
