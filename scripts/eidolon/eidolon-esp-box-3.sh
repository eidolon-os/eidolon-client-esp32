#!/usr/bin/env bash
# Eidolon dev toolkit - Espressif ESP-BOX-3
#
# Usage:
#   ./scripts/eidolon/eidolon-esp-box-3.sh build
#   ./scripts/eidolon/eidolon-esp-box-3.sh flash
#   ./scripts/eidolon/eidolon-esp-box-3.sh monitor
#
# Environment:
#   EIDOLON_PORT                       Serial port, for example /dev/cu.usbmodem1101
#   EIDOLON_IDF_VERSION                Override the ESP-IDF version this board
#                                      requires (default BOARD_IDF_VERSION)
#   EIDOLON_IDF_EXPORT                 Full path to ESP-IDF export.sh
#   EIDOLON_IDF_PATH                   ESP-IDF root directory
#   EIDOLON_OWNER_PRESENCE_VOICE_WAKE Enable owner-confirmed voice join: y/n (default y)
#   EIDOLON_BOX3_RADAR_THRESHOLD_DELTA Radar threshold: 0-1023, larger is nearer (default 450)
#   EIDOLON_RUNTIME_DIAGNOSTICS       Strong stack guards/watchpoint: y/n (default n)
#   EIDOLON_PRIVATE_SDKCONFIG_OVERLAY Absolute path to a private setup-secret
#                                      overlay (strict validation; see usage)
#   IDF_PATH                           ESP-IDF root directory

set -euo pipefail

readonly BOARD_PATH="esp-box-3"
readonly BOARD_NAME="esp-box-3"
readonly BOARD_TARGET="esp32s3"
readonly BOARD_IDF_VERSION="5.5.4"
readonly PUBLIC_BUILD_DIR="build/eidolon/esp-box-3"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/${PUBLIC_BUILD_DIR}"
SDKCONFIG_FILE="${BUILD_DIR}/sdkconfig.esp-box-3"
SDKCONFIG_OVERLAY="${BUILD_DIR}/sdkconfig.overlay.esp-box-3"
PRIVATE_SDKCONFIG_OVERLAY=""
PORT="${EIDOLON_PORT:-}"
OWNER_PRESENCE_VOICE_WAKE="${EIDOLON_OWNER_PRESENCE_VOICE_WAKE:-y}"
RADAR_THRESHOLD_DELTA="${EIDOLON_BOX3_RADAR_THRESHOLD_DELTA:-450}"
RUNTIME_DIAGNOSTICS="${EIDOLON_RUNTIME_DIAGNOSTICS:-n}"

if [[ "${OWNER_PRESENCE_VOICE_WAKE}" != "y" &&
      "${OWNER_PRESENCE_VOICE_WAKE}" != "n" ]]; then
  echo "error: EIDOLON_OWNER_PRESENCE_VOICE_WAKE must be y or n" >&2
  exit 2
fi
if [[ ! "${RADAR_THRESHOLD_DELTA}" =~ ^[0-9]+$ ]] ||
   ((RADAR_THRESHOLD_DELTA < 0 || RADAR_THRESHOLD_DELTA > 1023)); then
  echo "error: EIDOLON_BOX3_RADAR_THRESHOLD_DELTA must be an integer from 0 to 1023" >&2
  exit 2
fi
if [[ "${RUNTIME_DIAGNOSTICS}" != "y" &&
      "${RUNTIME_DIAGNOSTICS}" != "n" ]]; then
  echo "error: EIDOLON_RUNTIME_DIAGNOSTICS must be y or n" >&2
  exit 2
fi

# Shared helper: SDK version pin + forced re-resolution, build fingerprint,
# post-flash serial verification. See scripts/eidolon/eidolon-common.sh.
# shellcheck source=/dev/null
source "${SCRIPT_DIR}/eidolon-common.sh"

die() {
  echo "error: $*" >&2
  exit 1
}

info() {
  echo ">> $*"
}

stat_uid() {
  if stat -f '%u' "$1" >/dev/null 2>&1; then
    stat -f '%u' "$1"
  else
    stat -c '%u' "$1"
  fi
}

stat_mode() {
  if stat -f '%Lp' "$1" >/dev/null 2>&1; then
    stat -f '%Lp' "$1"
  else
    stat -c '%a' "$1"
  fi
}

stat_identity() {
  if stat -f '%d:%i' "$1" >/dev/null 2>&1; then
    stat -f '%d:%i' "$1"
  else
    stat -c '%d:%i' "$1"
  fi
}

canonical_existing_path() {
  local input="$1"
  local directory base
  directory="$(cd "$(dirname "${input}")" && pwd -P)"
  base="$(basename "${input}")"
  printf '%s/%s\n' "${directory}" "${base}"
}

configure_private_sdkconfig_overlay() {
  local requested="${EIDOLON_PRIVATE_SDKCONFIG_OVERLAY:-}"
  [[ -n "${requested}" ]] || return 0

  [[ "${requested}" == /* ]] ||
    die "EIDOLON_PRIVATE_SDKCONFIG_OVERLAY must be an absolute path"
  local requested_parent resolved_parent
  requested_parent="$(dirname "${requested}")"
  [[ -d "${requested_parent}" && ! -L "${requested_parent}" ]] ||
    die "private sdkconfig overlay parent must be a non-symlink directory"
  resolved_parent="$(cd "${requested_parent}" && pwd -P)"
  [[ "${requested_parent}" == "${resolved_parent}" ]] ||
    die "private sdkconfig overlay parent must not traverse symlinks"
  [[ "$(stat_uid "${resolved_parent}")" == "$(id -u)" ]] ||
    die "private sdkconfig overlay parent must be owned by the calling user"
  [[ "$(stat_mode "${resolved_parent}")" == "700" ]] ||
    die "private sdkconfig overlay parent permissions must be 0700"

  [[ -f "${requested}" && ! -L "${requested}" ]] ||
    die "private sdkconfig overlay must be a regular, non-symlink file"

  local resolved file_uid file_mode file_size setting identity_before identity_after
  resolved="$(canonical_existing_path "${requested}")"
  case "${resolved}" in
    "${PROJECT_ROOT}"|"${PROJECT_ROOT}"/*)
      die "private sdkconfig overlay must be outside the repository"
      ;;
  esac

  file_uid="$(stat_uid "${resolved}")"
  [[ "${file_uid}" == "$(id -u)" ]] ||
    die "private sdkconfig overlay must be owned by the calling user"
  file_mode="$(stat_mode "${resolved}")"
  [[ "${file_mode}" == "600" ]] ||
    die "private sdkconfig overlay permissions must be 0600"
  identity_before="$(stat_identity "${resolved}")"
  file_size="$(wc -c <"${resolved}" | tr -d '[:space:]')"
  [[ "${file_size}" =~ ^[0-9]+$ ]] &&
    ((file_size > 0 && file_size <= 256)) ||
    die "private sdkconfig overlay has an invalid size"

  setting="$(cat "${resolved}")"
  identity_after="$(stat_identity "${resolved}")"
  [[ "${identity_before}" == "${identity_after}" ]] ||
    die "private sdkconfig overlay changed during validation"
  [[ "${setting}" =~ ^CONFIG_EIDOLON_ADMISSION_SETUP_SECRET_HEX=\"[0-9a-f]{64}\"$ ]] ||
    die "private sdkconfig overlay must contain exactly the supported setup-secret setting"
  local setting_size="${#setting}"
  ((file_size == setting_size || file_size == setting_size + 1)) ||
    die "private sdkconfig overlay must contain exactly one setting line"

  local private_root="${resolved}.build"
  if [[ -e "${private_root}" ]]; then
    [[ -d "${private_root}" && ! -L "${private_root}" ]] ||
      die "private build root must be a non-symlink directory"
    [[ "$(stat_uid "${private_root}")" == "$(id -u)" ]] ||
      die "private build root must be owned by the calling user"
  fi
  local private_build="${private_root}/esp-box-3"
  if [[ -e "${private_build}" ]]; then
    [[ -d "${private_build}" && ! -L "${private_build}" ]] ||
      die "private build directory must be a non-symlink directory"
    [[ "$(stat_uid "${private_build}")" == "$(id -u)" ]] ||
      die "private build directory must be owned by the calling user"
  fi
  (umask 077; mkdir -p "${private_build}")
  chmod 0700 "${private_root}" "${private_build}"
  [[ "$(stat_uid "${resolved_parent}")" == "$(id -u)" &&
     "$(stat_mode "${resolved_parent}")" == "700" &&
     ! -L "${resolved_parent}" ]] ||
    die "private sdkconfig overlay parent changed during validation"
  [[ "$(stat_uid "${private_root}")" == "$(id -u)" &&
     "$(stat_mode "${private_root}")" == "700" &&
     ! -L "${private_root}" ]] ||
    die "private build root failed secure revalidation"
  [[ "$(stat_uid "${private_build}")" == "$(id -u)" &&
     "$(stat_mode "${private_build}")" == "700" &&
     ! -L "${private_build}" ]] ||
    die "private build directory failed secure revalidation"

  # IDF reads this sealed snapshot, not the caller path. A rename after
  # validation therefore cannot change the secret that reaches Kconfig.
  local sealed_overlay="${private_root}/sdkconfig.private.esp-box-3"
  local sealed_temporary="${sealed_overlay}.tmp.$$"
  (umask 077; printf '%s\n' "${setting}" >"${sealed_temporary}")
  chmod 0600 "${sealed_temporary}"
  mv "${sealed_temporary}" "${sealed_overlay}"
  [[ -f "${sealed_overlay}" && ! -L "${sealed_overlay}" &&
     "$(stat_uid "${sealed_overlay}")" == "$(id -u)" &&
     "$(stat_mode "${sealed_overlay}")" == "600" ]] ||
    die "sealed private sdkconfig overlay failed secure revalidation"

  PRIVATE_SDKCONFIG_OVERLAY="${sealed_overlay}"
  BUILD_DIR="${private_build}"
  SDKCONFIG_FILE="${BUILD_DIR}/sdkconfig.esp-box-3"
  SDKCONFIG_OVERLAY="${BUILD_DIR}/sdkconfig.overlay.esp-box-3"
}

# ESP-IDF selection lives in eidolon-common.sh and is keyed on
# BOARD_IDF_VERSION above: resolved by version, verified after export, and baked
# into the build stamp. Never "whatever IDF happens to be newest on this box".
idf_ready() {
  command -v idf.py >/dev/null 2>&1
}

ensure_idf_env() {
  eidolon_idf_ensure "${BOARD_IDF_VERSION}"
}

require_idf() {
  eidolon_require_idf "${BOARD_IDF_VERSION}"
}

list_ports() {
  local -a patterns=(
    /dev/cu.usbmodem*
    /dev/cu.wchusbserial*
    /dev/cu.SLAB_USBtoUART*
    /dev/cu.usbserial*
    /dev/ttyUSB*
    /dev/ttyACM*
  )
  local p
  for p in "${patterns[@]}"; do
    [[ -e "${p}" ]] && echo "${p}"
  done
}

detect_port() {
  if [[ -n "${PORT}" ]]; then
    echo "${PORT}"
    return 0
  fi

  local -a ports=()
  while IFS= read -r line; do
    [[ -n "${line}" ]] && ports+=("${line}")
  done < <(list_ports | sort -u)

  if ((${#ports[@]} == 0)); then
    die "No USB serial port found. Set EIDOLON_PORT=/dev/cu.xxx"
  fi
  if ((${#ports[@]} > 1)); then
    printf 'Multiple serial ports found; set EIDOLON_PORT explicitly:\n' >&2
    printf '  %s\n' "${ports[@]}" >&2
    exit 1
  fi
  echo "${ports[0]}"
}

write_overlay() {
  mkdir -p "${BUILD_DIR}"
  cat >"${SDKCONFIG_OVERLAY}" <<EOF
# Generated by scripts/eidolon/eidolon-esp-box-3.sh
CONFIG_BOARD_TYPE_ESP_BOX_3=y
CONFIG_USE_AUDIO_PROCESSOR=y
CONFIG_USE_DEVICE_AEC=y
CONFIG_USE_SERVER_AEC=n
CONFIG_EIDOLON_DEVICE_AEC_AFE_MODE_LOW_COST=y
# CONFIG_EIDOLON_DEVICE_AEC_AFE_MODE_HIGH_PERF is not set
CONFIG_EIDOLON_HUB_MODE=y
CONFIG_EIDOLON_AUTO_JOIN_ON_ACTIVATION=n
CONFIG_EIDOLON_DEV_DISABLE_AUTO_SHUTDOWN=y
CONFIG_EIDOLON_RADAR_PRESENCE_BROADCAST=y
CONFIG_EIDOLON_BOX3_RADAR_THRESHOLD_DELTA=${RADAR_THRESHOLD_DELTA}
CONFIG_EIDOLON_OWNER_PRESENCE_VOICE_WAKE=${OWNER_PRESENCE_VOICE_WAKE}
# CONFIG_EIDOLON_INTERACTION_MODE_PTT is not set
# CONFIG_EIDOLON_INTERACTION_MODE_HALF_DUPLEX is not set
CONFIG_EIDOLON_FULL_DUPLEX_IDLE_FALLBACK_MS=75000
CONFIG_USE_EMOTE_MESSAGE_STYLE=y
# CONFIG_USE_DEFAULT_MESSAGE_STYLE is not set
# CONFIG_USE_WECHAT_MESSAGE_STYLE is not set
# CONFIG_FLASH_DEFAULT_ASSETS is not set
CONFIG_FLASH_EXPRESSION_ASSETS=y
# CONFIG_EIDOLON_WAKE_WORD_ENABLE is not set
CONFIG_WAKE_WORD_DISABLED=y
# CONFIG_USE_ESP_WAKE_WORD is not set
# CONFIG_USE_AFE_WAKE_WORD is not set
# CONFIG_USE_CUSTOM_WAKE_WORD is not set
CONFIG_EIDOLON_LIVEKIT_SPEAKER_VOLUME=80
CONFIG_MMAP_FILE_NAME_LENGTH=32
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/v2/16m_eidolon_box3.csv"
CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES=y
CONFIG_MDNS_MAX_SERVICES=10
CONFIG_ESP_WS_CLIENT_ENABLE_DYNAMIC_BUFFER=y
CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK=y
CONFIG_MBEDTLS_SSL_DTLS_SRTP=y
CONFIG_MBEDTLS_SSL_PROTO_DTLS=y
EOF
}

set_sdkconfig_bool() {
  local key="$1"
  local value="$2"
  local sdkconfig="${SDKCONFIG_FILE}"
  local tmp="${sdkconfig}.tmp"

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
  local key="$1"
  local value="$2"
  local sdkconfig="${SDKCONFIG_FILE}"
  local tmp="${sdkconfig}.tmp"

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

ensure_box3_sdkconfig() {
  set_sdkconfig_bool BOARD_TYPE_ESP_BOX_3 y
  set_sdkconfig_bool EIDOLON_HUB_MODE y
  set_sdkconfig_bool EIDOLON_AUTO_JOIN_ON_ACTIVATION n
  set_sdkconfig_bool EIDOLON_DEV_DISABLE_AUTO_SHUTDOWN y
  set_sdkconfig_bool EIDOLON_RADAR_PRESENCE_BROADCAST y
  set_sdkconfig_value EIDOLON_BOX3_RADAR_THRESHOLD_DELTA "${RADAR_THRESHOLD_DELTA}"
  set_sdkconfig_bool EIDOLON_OWNER_PRESENCE_VOICE_WAKE "${OWNER_PRESENCE_VOICE_WAKE}"
  set_sdkconfig_bool EIDOLON_INTERACTION_MODE_PTT n
  set_sdkconfig_bool USE_DEVICE_AEC y
  set_sdkconfig_bool EIDOLON_INTERACTION_MODE_HALF_DUPLEX n

  set_sdkconfig_bool USE_DEFAULT_MESSAGE_STYLE n
  set_sdkconfig_bool USE_WECHAT_MESSAGE_STYLE n
  set_sdkconfig_bool USE_EMOTE_MESSAGE_STYLE y
  set_sdkconfig_bool FLASH_NONE_ASSETS n
  set_sdkconfig_bool FLASH_DEFAULT_ASSETS n
  set_sdkconfig_bool FLASH_CUSTOM_ASSETS n
  set_sdkconfig_bool FLASH_EXPRESSION_ASSETS y

  set_sdkconfig_bool EIDOLON_WAKE_WORD_ENABLE n
  set_sdkconfig_bool WAKE_WORD_DISABLED y
  set_sdkconfig_bool USE_ESP_WAKE_WORD n
  set_sdkconfig_bool USE_AFE_WAKE_WORD n
  set_sdkconfig_bool USE_CUSTOM_WAKE_WORD n

  set_sdkconfig_value EIDOLON_LIVEKIT_SPEAKER_VOLUME 80
  set_sdkconfig_value EIDOLON_FULL_DUPLEX_IDLE_FALLBACK_MS 75000
  set_sdkconfig_value MMAP_FILE_NAME_LENGTH 32
  set_sdkconfig_value PARTITION_TABLE_CUSTOM_FILENAME '"partitions/v2/16m_eidolon_box3.csv"'

  # Reproducible, opt-in crash diagnostics. Keep this out of normal firmware:
  # the guards deliberately trade code size for exact evidence. Explicitly
  # restoring every choice prevents a later normal build from silently
  # inheriting a diagnostic sdkconfig.
  if [[ "${RUNTIME_DIAGNOSTICS}" == "y" ]]; then
    set_sdkconfig_bool COMPILER_STACK_CHECK_MODE_NONE n
    set_sdkconfig_bool COMPILER_STACK_CHECK_MODE_NORM n
    set_sdkconfig_bool COMPILER_STACK_CHECK_MODE_STRONG y
    set_sdkconfig_bool COMPILER_STACK_CHECK_MODE_ALL n
    set_sdkconfig_bool COMPILER_STACK_CHECK y
    set_sdkconfig_bool FREERTOS_WATCHPOINT_END_OF_STACK y
  else
    set_sdkconfig_bool COMPILER_STACK_CHECK_MODE_NONE y
    set_sdkconfig_bool COMPILER_STACK_CHECK_MODE_NORM n
    set_sdkconfig_bool COMPILER_STACK_CHECK_MODE_STRONG n
    set_sdkconfig_bool COMPILER_STACK_CHECK_MODE_ALL n
    set_sdkconfig_bool COMPILER_STACK_CHECK n
    set_sdkconfig_bool FREERTOS_WATCHPOINT_END_OF_STACK n
  fi
}

idf_args() {
  local defaults=(
    "${PROJECT_ROOT}/sdkconfig.defaults"
    "${PROJECT_ROOT}/sdkconfig.defaults.${BOARD_TARGET}"
    "${SDKCONFIG_OVERLAY}"
  )
  if [[ -n "${PRIVATE_SDKCONFIG_OVERLAY}" ]]; then
    defaults+=("${PRIVATE_SDKCONFIG_OVERLAY}")
  fi
  local joined=""
  local file
  for file in "${defaults[@]}"; do
    [[ -f "${file}" ]] || continue
    [[ -n "${joined}" ]] && joined+=";"
    joined+="${file}"
  done

  printf '%s\n' \
    -B "${BUILD_DIR}" \
    -DIDF_TARGET="${BOARD_TARGET}" \
    -DSDKCONFIG="${SDKCONFIG_FILE}" \
    -DSDKCONFIG_DEFAULTS="${joined}" \
    -DBOARD_NAME="${BOARD_NAME}" \
    -DBOARD_TYPE="${BOARD_PATH}"
}

run_idf() {
  require_idf
  write_overlay
  if [[ -n "${PRIVATE_SDKCONFIG_OVERLAY}" ]]; then
    # Never let a previous private build pin a stale setup secret. The current
    # validated overlay remains the only source; generated sdkconfig stays in
    # the repository-external 0700 build directory.
    rm -f "${SDKCONFIG_FILE}"
  fi
  ensure_box3_sdkconfig
  local -a base_args=()
  while IFS= read -r arg; do
    base_args+=("${arg}")
  done < <(idf_args)
  (cd "${PROJECT_ROOT}" && idf.py "${base_args[@]}" "$@")
}

usage() {
  cat <<EOF
Usage: $0 <command>

Commands:
  build       Build ESP-BOX-3 Eidolon firmware
  flash       Flash ESP-BOX-3 Eidolon firmware (auto-verifies the build stamp)
  monitor     Open idf.py monitor
  verify      Read the boot build stamp over serial and diff vs the last build
  clean       Remove ${BUILD_DIR}
  list-ports  Print detected serial ports

Environment:
  EIDOLON_PORT=/dev/cu.usbmodemXXXX
  EIDOLON_IDF_VERSION=5.5.4    Override the ESP-IDF version this board requires.
                               The board pins BOARD_IDF_VERSION and the build is
                               refused if the exported toolchain is anything else.
  EIDOLON_LIVEKIT_SDK=0.3.7   Pin the LiveKit SDK version (forces clean re-resolve)
  EIDOLON_OWNER_PRESENCE_VOICE_WAKE=y
                               Compile owner-confirmed automatic voice join (default y)
  EIDOLON_BOX3_RADAR_THRESHOLD_DELTA=450
                               Unitless AT581X threshold; larger is nearer
  EIDOLON_RUNTIME_DIAGNOSTICS=n
                               Enable reproducible strong stack diagnostics
  EIDOLON_PRIVATE_SDKCONFIG_OVERLAY=/absolute/private/path
                               Optional HIL-only file. It must be outside this
                               repository in a caller-owned 0700 real directory,
                               be caller-owned and mode 0600, and contain exactly
                               one 64-lowercase-hex setup-secret setting. Its
                               sealed build directory is external and 0700.
EOF
}

if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
  return 0
fi

configure_private_sdkconfig_overlay

cmd="${1:-build}"
case "${cmd}" in
  build)
    eidolon_prepare_build "${PROJECT_ROOT}"
    run_idf build
    ;;
  flash)
    PORT="$(detect_port)"
    info "Using serial port: ${PORT}"
    eidolon_prepare_build "${PROJECT_ROOT}"
    run_idf -p "${PORT}" flash
    eidolon_verify_flashed "${PROJECT_ROOT}" "${PORT}"
    ;;
  monitor)
    PORT="$(detect_port)"
    info "Using serial port: ${PORT}"
    run_idf -p "${PORT}" monitor
    ;;
  clean)
    rm -rf "${BUILD_DIR}"
    ;;
  list-ports)
    list_ports
    ;;
  verify)
    PORT="$(detect_port)"
    eidolon_verify_flashed "${PROJECT_ROOT}" "${PORT}"
    ;;
  -h|--help|help)
    usage
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac
