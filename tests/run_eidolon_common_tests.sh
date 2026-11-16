#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
source scripts/eidolon/eidolon-common.sh

fixture="$(mktemp -d "${TMPDIR:-/tmp}/eidolon_common_tests.XXXXXX")"
trap 'rm -rf "${fixture}"' EXIT
verify_root="${fixture}/verify"
fake_idf_python="${fixture}/idf-python"
mkdir -p "${verify_root}" "${fake_idf_python}/bin"
cat >"${verify_root}/.eidolon_expected_stamp" <<'EOF'
git=abcdef123 branch=test sdk=1234567890abcdef idf=6.1
EOF
cat >"${fake_idf_python}/bin/python" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "${EIDOLON_TEST_ACTUAL:-}"
EOF
chmod +x "${fake_idf_python}/bin/python"

matched="$(
  EIDOLON_TEST_ACTUAL=$'EIDOLON-STORAGE owner_trust=ready bytes=65536\nEIDOLON-BUILDSTAMP git=abcdef123 branch=test sdk=1234567890abcdef idf=6.1' \
    IDF_PYTHON_ENV_PATH="${fake_idf_python}" \
    eidolon_verify_flashed "${verify_root}" /dev/test 0 2>&1
)"
[[ "${matched}" == *"VERIFIED:"* ]] || {
  echo "matching build stamp was not accepted" >&2
  exit 1
}

if EIDOLON_TEST_ACTUAL='' IDF_PYTHON_ENV_PATH="${fake_idf_python}" \
  eidolon_verify_flashed "${verify_root}" /dev/test 0 >/dev/null 2>&1; then
  echo "missing build stamp was accepted" >&2
  exit 1
fi

if EIDOLON_TEST_ACTUAL=$'EIDOLON-STORAGE owner_trust=ready bytes=65536\nEIDOLON-BUILDSTAMP git=deadbeef0 branch=test sdk=1234567890abcdef idf=6.1' \
  IDF_PYTHON_ENV_PATH="${fake_idf_python}" \
  eidolon_verify_flashed "${verify_root}" /dev/test 0 >/dev/null 2>&1; then
  echo "mismatched build stamp was accepted" >&2
  exit 1
fi

if IDF_PYTHON_ENV_PATH='' \
  eidolon_verify_flashed "${verify_root}" /dev/test 0 >/dev/null 2>&1; then
  echo "verification accepted an unresolved IDF Python environment" >&2
  exit 1
fi

if EIDOLON_TEST_ACTUAL='EIDOLON-BUILDSTAMP git=abcdef123 branch=test sdk=1234567890abcdef idf=6.1' IDF_PYTHON_ENV_PATH="${fake_idf_python}" \
  eidolon_verify_flashed "${verify_root}" /dev/test 0 >/dev/null 2>&1; then
  echo "missing storage readiness was accepted" >&2
  exit 1
fi

for actual_board in waveshare stackchan missing; do
  status=0
  BOARD_NAME=waveshare \
    EIDOLON_TEST_ACTUAL=$'EIDOLON-STORAGE owner_trust=ready bytes=65536\nEIDOLON-BUILDSTAMP git=abcdef123 branch=test sdk=1234567890abcdef idf=6.1\nEIDOLON-BOARD sku='"${actual_board}" \
    IDF_PYTHON_ENV_PATH="${fake_idf_python}" \
    eidolon_verify_flashed "${verify_root}" /dev/test 0 >/dev/null 2>&1 || status=$?
  if [[ "${actual_board}" == waveshare ]]; then
    [[ "${status}" == 0 ]] || { echo "matching board was rejected" >&2; exit 1; }
  else
    [[ "${status}" != 0 ]] || { echo "wrong board was accepted" >&2; exit 1; }
  fi
done

echo "eidolon_common_test: PASS"
