#!/usr/bin/env bash
set -euo pipefail

readonly BOARD_PATH="wwl-ball-s3-lcd-1.85"
readonly BOARD_NAME="wwl-ball-s3-lcd-1.85"
readonly BOARD_TARGET="esp32s3"
readonly BOARD_IDF_VERSION="5.5.4"
readonly PUBLIC_BUILD_DIR="build/eidolon/wwl-ball-s3-lcd-1.85"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/${PUBLIC_BUILD_DIR}"
SDKCONFIG_FILE="${BUILD_DIR}/sdkconfig.wwl-ball-s3-lcd-1.85"
SDKCONFIG_OVERLAY="${BUILD_DIR}/sdkconfig.overlay.wwl-ball-s3-lcd-1.85"
PORT="${EIDOLON_PORT:-}"
INTERACTION_MODE="${EIDOLON_INTERACTION_MODE:-ptt}"
RUNTIME_DIAGNOSTICS="${EIDOLON_RUNTIME_DIAGNOSTICS:-n}"

case "${INTERACTION_MODE}" in
  half_duplex) PTT_ENABLED=n; HALF_DUPLEX_ENABLED=y ;;
  ptt) PTT_ENABLED=y; HALF_DUPLEX_ENABLED=n ;;
  *) echo "error: EIDOLON_INTERACTION_MODE must be half_duplex or ptt" >&2; exit 2 ;;
esac

if [[ "${RUNTIME_DIAGNOSTICS}" != "y" && "${RUNTIME_DIAGNOSTICS}" != "n" ]]; then
  echo "error: EIDOLON_RUNTIME_DIAGNOSTICS must be y or n" >&2
  exit 2
fi

source "${SCRIPT_DIR}/eidolon-common.sh"

set_sdkconfig_bool() {
  local key="$1" value="$2" sdkconfig="${SDKCONFIG_FILE}" tmp="${SDKCONFIG_FILE}.tmp"
  mkdir -p "$(dirname "${sdkconfig}")"
  touch "${sdkconfig}"
  awk -v key="${key}" '
    $0 == "CONFIG_" key "=y" { next }
    $0 == "CONFIG_" key "=n" { next }
    $0 == "# CONFIG_" key " is not set" { next }
    { print }
  ' "${sdkconfig}" >"${tmp}"
  if [[ "${value}" == "y" ]]; then
    printf 'CONFIG_%s=y\n' "${key}" >>"${tmp}"
  else
    printf '# CONFIG_%s is not set\n' "${key}" >>"${tmp}"
  fi
  mv "${tmp}" "${sdkconfig}"
}

set_sdkconfig_value() {
  local key="$1" value="$2" sdkconfig="${SDKCONFIG_FILE}" tmp="${SDKCONFIG_FILE}.tmp"
  mkdir -p "$(dirname "${sdkconfig}")"
  touch "${sdkconfig}"
  awk -v key="${key}" '
    index($0, "CONFIG_" key "=") == 1 { next }
    $0 == "# CONFIG_" key " is not set" { next }
    { print }
  ' "${sdkconfig}" >"${tmp}"
  printf 'CONFIG_%s=%s\n' "${key}" "${value}" >>"${tmp}"
  mv "${tmp}" "${sdkconfig}"
}

write_overlay() {
  mkdir -p "${BUILD_DIR}"
  cat >"${SDKCONFIG_OVERLAY}" <<EOF
CONFIG_BOARD_TYPE_WWL_BALL_S3_LCD_1_85=y
CONFIG_EIDOLON_HUB_MODE=y
CONFIG_EIDOLON_AUTO_JOIN_ON_ACTIVATION=n
CONFIG_EIDOLON_DEV_DISABLE_AUTO_SHUTDOWN=y
CONFIG_EIDOLON_INTERACTION_MODE_PTT=${PTT_ENABLED}
CONFIG_EIDOLON_INTERACTION_MODE_HALF_DUPLEX=${HALF_DUPLEX_ENABLED}
CONFIG_EIDOLON_UI_TALK_BUTTON=y
CONFIG_USE_AUDIO_PROCESSOR=n
CONFIG_USE_DEVICE_AEC=n
CONFIG_USE_SERVER_AEC=n
# CONFIG_USE_DEFAULT_MESSAGE_STYLE is not set
CONFIG_USE_EMOTE_MESSAGE_STYLE=y
# CONFIG_USE_WECHAT_MESSAGE_STYLE is not set
# CONFIG_FLASH_DEFAULT_ASSETS is not set
# CONFIG_FLASH_CUSTOM_ASSETS is not set
# CONFIG_FLASH_NONE_ASSETS is not set
CONFIG_FLASH_EXPRESSION_ASSETS=y
# CONFIG_EIDOLON_WAKE_WORD_ENABLE is not set
CONFIG_WAKE_WORD_DISABLED=y
# CONFIG_USE_ESP_WAKE_WORD is not set
# CONFIG_USE_AFE_WAKE_WORD is not set
# CONFIG_USE_CUSTOM_WAKE_WORD is not set
CONFIG_EIDOLON_LIVEKIT_SPEAKER_VOLUME=80
CONFIG_MMAP_FILE_NAME_LENGTH=32
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/v2/16m_eidolon.csv"
CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES=y
CONFIG_MDNS_MAX_SERVICES=10
CONFIG_ESP_WS_CLIENT_ENABLE_DYNAMIC_BUFFER=y
CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK=y
CONFIG_MBEDTLS_SSL_DTLS_SRTP=y
CONFIG_MBEDTLS_SSL_PROTO_DTLS=y
EOF
}

ensure_wwl_sdkconfig() {
  set_sdkconfig_bool BOARD_TYPE_WWL_BALL_S3_LCD_1_85 y
  set_sdkconfig_bool EIDOLON_HUB_MODE y
  set_sdkconfig_bool EIDOLON_AUTO_JOIN_ON_ACTIVATION n
  set_sdkconfig_bool EIDOLON_DEV_DISABLE_AUTO_SHUTDOWN y
  set_sdkconfig_bool EIDOLON_INTERACTION_MODE_PTT "${PTT_ENABLED}"
  set_sdkconfig_bool EIDOLON_INTERACTION_MODE_HALF_DUPLEX "${HALF_DUPLEX_ENABLED}"
  set_sdkconfig_bool EIDOLON_UI_TALK_BUTTON y
  set_sdkconfig_bool USE_AUDIO_PROCESSOR n
  set_sdkconfig_bool USE_DEVICE_AEC n
  set_sdkconfig_bool USE_SERVER_AEC n
  set_sdkconfig_bool USE_DEFAULT_MESSAGE_STYLE n
  set_sdkconfig_bool USE_EMOTE_MESSAGE_STYLE y
  set_sdkconfig_bool USE_WECHAT_MESSAGE_STYLE n
  set_sdkconfig_bool FLASH_DEFAULT_ASSETS n
  set_sdkconfig_bool FLASH_CUSTOM_ASSETS n
  set_sdkconfig_bool FLASH_NONE_ASSETS n
  set_sdkconfig_bool FLASH_EXPRESSION_ASSETS y
  set_sdkconfig_bool EIDOLON_WAKE_WORD_ENABLE n
  set_sdkconfig_bool WAKE_WORD_DISABLED y
  set_sdkconfig_bool USE_ESP_WAKE_WORD n
  set_sdkconfig_bool USE_AFE_WAKE_WORD n
  set_sdkconfig_bool USE_CUSTOM_WAKE_WORD n
  set_sdkconfig_value EIDOLON_LIVEKIT_SPEAKER_VOLUME 80
  set_sdkconfig_value MMAP_FILE_NAME_LENGTH 32
  set_sdkconfig_value PARTITION_TABLE_CUSTOM_FILENAME '"partitions/v2/16m_eidolon.csv"'
  set_sdkconfig_bool COMPILER_STACK_CHECK_MODE_NONE "$([[ "${RUNTIME_DIAGNOSTICS}" == y ]] && echo n || echo y)"
}

idf_args() {
  local defaults=("${PROJECT_ROOT}/sdkconfig.defaults" "${PROJECT_ROOT}/sdkconfig.defaults.${BOARD_TARGET}" "${SDKCONFIG_OVERLAY}") joined="" file
  for file in "${defaults[@]}"; do
    [[ -f "${file}" ]] || continue
    [[ -n "${joined}" ]] && joined+=';'
    joined+="${file}"
  done
  printf '%s\n' -B "${BUILD_DIR}" -DIDF_TARGET="${BOARD_TARGET}" \
    -DSDKCONFIG="${SDKCONFIG_FILE}" -DSDKCONFIG_DEFAULTS="${joined}" \
    -DBOARD_NAME="${BOARD_NAME}" -DBOARD_TYPE="${BOARD_PATH}"
}

run_idf() {
  eidolon_require_idf "${BOARD_IDF_VERSION}"
  write_overlay
  ensure_wwl_sdkconfig
  local -a args=() arg
  while IFS= read -r arg; do args+=("${arg}"); done < <(idf_args)
  (cd "${PROJECT_ROOT}" && idf.py "${args[@]}" "$@")
}

detect_port() {
  if [[ -n "${PORT}" ]]; then printf '%s\n' "${PORT}"; return; fi
  local -a ports=(/dev/ttyACM* /dev/ttyUSB*)
  local -a existing=() port
  for port in "${ports[@]}"; do [[ -e "${port}" ]] && existing+=("${port}"); done
  [[ "${#existing[@]}" -eq 1 ]] || { echo "error: set EIDOLON_PORT explicitly" >&2; return 1; }
  printf '%s\n' "${existing[0]}"
}

usage() {
  printf '%s\n' "Usage: $0 {build|flash|monitor|verify|clean|list-ports}" \
    "EIDOLON_PORT=/dev/ttyACM0" \
    "EIDOLON_INTERACTION_MODE=ptt|half_duplex"
}

if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
  return 0
fi

case "${1:-build}" in
  build) eidolon_prepare_build "${PROJECT_ROOT}"; run_idf build ;;
  flash) PORT="$(detect_port)"; eidolon_prepare_build "${PROJECT_ROOT}"; run_idf -p "${PORT}" flash -b 115200; eidolon_verify_flashed "${PROJECT_ROOT}" "${PORT}" ;;
  monitor) PORT="$(detect_port)"; run_idf -p "${PORT}" monitor ;;
  verify) PORT="$(detect_port)"; eidolon_verify_flashed "${PROJECT_ROOT}" "${PORT}" ;;
  clean) rm -rf "${BUILD_DIR}" ;;
  list-ports) printf '%s\n' /dev/ttyACM* /dev/ttyUSB* 2>/dev/null ;;
  -h|--help|help) usage ;;
  *) usage >&2; exit 2 ;;
esac
