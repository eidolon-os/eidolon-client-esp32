#!/usr/bin/env bash
# The device admission chain, against the real rig.
#
# Default (read-only) — reports on the rig as it stands, drives nothing:
#   tests/hil/run_device_admission_hil.sh
#
# The whole chain, which re-provisions and then removes the device:
#   tests/hil/run_device_admission_hil.sh \
#     --stages rig,image,add,authority,removal --wifi-password '<owner wifi>'
#
# It needs exclusive use of the board's serial line. If another session is
# flashing or monitoring it, the run reports BLOCKED rather than guessing.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo_root"
exec python3 tests/hil/device_admission_hil.py "$@"
