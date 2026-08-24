#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

python_bin="${PYTHON:-python3}"
if ! "${python_bin}" -c 'import pytest' >/dev/null 2>&1; then
  sdk_python="${repo_root}/../eidolon_sdk/.venv/bin/python"
  [[ -x "${sdk_python}" ]] || {
    echo "error: pytest unavailable; set PYTHON to a pytest-capable interpreter" >&2
    exit 1
  }
  python_bin="${sdk_python}"
fi

"${python_bin}" -m pytest -q tests/private_sdkconfig_overlay_test.py
