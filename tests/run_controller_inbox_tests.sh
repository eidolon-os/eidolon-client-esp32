#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
: "${IDF_PATH:?Source ESP-IDF export.sh first}"
output="$PWD/.cache/controller-inbox-build"
# This test has no registry dependencies.
export IDF_COMPONENT_MANAGER=0
idf.py -C tests/controller_inbox_idf -B "$output" \
  -D SDKCONFIG="$PWD/.cache/controller-inbox-sdkconfig" build merge-bin
python3 tests/controller_inbox_idf/run_qemu.py "$output/merged-binary.bin" \
  "$PWD/.cache/controller-inbox-qemu.log"
