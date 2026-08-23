#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
source scripts/eidolon/eidolon-common.sh

fixture="$(mktemp -d "${TMPDIR:-/tmp}/eidolon_common_tests.XXXXXX")"
trap 'rm -rf "${fixture}"' EXIT
mkdir -p "${fixture}/main" "${fixture}/managed_components/livekit__livekit"

cat >"${fixture}/main/idf_component.yml" <<'EOF'
dependencies:
  livekit/livekit:
    git: https://github.com/eidolon-os/client-sdk-esp32.git
    path: components/livekit
    version: 502ce391bf01c47dea74e1dd651c5b8945ca8906
EOF
cat >"${fixture}/managed_components/livekit__livekit/idf_component.yml" <<'EOF'
version: 0.3.10~1
EOF
cat >"${fixture}/dependencies.lock" <<'EOF'
dependencies:
  livekit/livekit:
    component_hash: test-only
    source:
      git: https://github.com/eidolon-os/client-sdk-esp32.git
      path: components/livekit
      type: git
    version: 502ce391bf01c47dea74e1dd651c5b8945ca8906
manifest_hash: test-only
target: esp32s3
version: 2.0.0
EOF

expected="502ce391bf01c47dea74e1dd651c5b8945ca8906"
actual="$(eidolon_installed_sdk "${fixture}")"
[[ "${actual}" == "${expected}" ]] || {
  echo "resolved SDK mismatch: expected=${expected} actual=${actual}" >&2
  exit 1
}

echo "eidolon_common_test: PASS"
