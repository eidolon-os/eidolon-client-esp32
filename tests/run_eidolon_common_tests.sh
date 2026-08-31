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

verify_root="${fixture}/verify"
fake_bin="${fixture}/bin"
mkdir -p "${verify_root}" "${fake_bin}"
cat >"${verify_root}/.eidolon_expected_stamp" <<'EOF'
git=abcdef123 branch=test sdk=1234567890abcdef idf=6.1
EOF
cat >"${fake_bin}/python3" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "${EIDOLON_TEST_ACTUAL:-}"
EOF
chmod +x "${fake_bin}/python3"

matched="$(
  EIDOLON_TEST_ACTUAL='EIDOLON-BUILDSTAMP git=abcdef123 branch=test sdk=1234567890abcdef idf=6.1' \
    PATH="${fake_bin}:${PATH}" \
    eidolon_verify_flashed "${verify_root}" /dev/test 0 2>&1
)"
[[ "${matched}" == *"VERIFIED:"* ]] || {
  echo "matching build stamp was not accepted" >&2
  exit 1
}

if EIDOLON_TEST_ACTUAL='' PATH="${fake_bin}:${PATH}" \
  eidolon_verify_flashed "${verify_root}" /dev/test 0 >/dev/null 2>&1; then
  echo "missing build stamp was accepted" >&2
  exit 1
fi

if EIDOLON_TEST_ACTUAL='EIDOLON-BUILDSTAMP git=deadbeef0 branch=test sdk=1234567890abcdef idf=6.1' \
  PATH="${fake_bin}:${PATH}" \
  eidolon_verify_flashed "${verify_root}" /dev/test 0 >/dev/null 2>&1; then
  echo "mismatched build stamp was accepted" >&2
  exit 1
fi

echo "eidolon_common_test: PASS"
