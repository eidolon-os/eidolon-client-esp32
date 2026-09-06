#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
# The lock's shape first: it needs no SDK checkout, and a malformed lock should
# be reported as a malformed lock rather than as a git failure downstream of it.
python3 tests/device_foundation_lock_shape_test.py
python3 scripts/sync_device_foundation_v1.py --check
