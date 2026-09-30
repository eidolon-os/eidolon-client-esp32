#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
: "${IDF_PATH:?Source ESP-IDF export.sh first}"
output="$PWD/.cache/firmware-boot-build"
export IDF_COMPONENT_MANAGER=0
idf.py -C tests/firmware_boot_idf -B "$output" \
  -D SDKCONFIG="$PWD/.cache/firmware-boot-sdkconfig" build
python3 tests/firmware_boot_idf/run_qemu.py "$output"
