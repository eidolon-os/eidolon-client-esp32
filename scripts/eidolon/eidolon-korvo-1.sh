#!/usr/bin/env bash
# Eidolon dev toolkit - Espressif ESP32-S31-Korvo-1 (smart home panel)
#
# Usage:
#   ./scripts/eidolon/eidolon-korvo-1.sh build
#   ./scripts/eidolon/eidolon-korvo-1.sh flash
#   ./scripts/eidolon/eidolon-korvo-1.sh monitor
#   ./scripts/eidolon/eidolon-korvo-1.sh size
#
# The board is declared here, not copied: its sdkconfig comes from
# main/boards/korvo-1/config.json (the list scripts/release.py builds from), and
# its serial port is found by the USB bridge its console sits behind.
#
# Environment:
#   EIDOLON_PORT         Serial port to use as-is, e.g. /dev/cu.usbserial-2140
#   EIDOLON_USB_SERIAL   Pick one Korvo-1 by its CP2102N serial number when
#                        more than one is plugged in
#   EIDOLON_IDF_VERSION  Override the ESP-IDF version this board requires
#   EIDOLON_IDF_PATH / EIDOLON_IDF_EXPORT   Where an ESP-IDF lives

set -euo pipefail

readonly BOARD_PATH="korvo-1"
readonly BOARD_NAME="korvo-1"
readonly BOARD_KCONFIG="CONFIG_BOARD_TYPE_KORVO_1"
readonly BOARD_TARGET="esp32s31"
# esp32s31 exists only from ESP-IDF 6.1; release/v6.0 has no components/soc entry
# for it. Declared here so eidolon-common.sh resolves and verifies that exact
# toolchain instead of whatever is newest on the machine.
readonly BOARD_IDF_VERSION="6.1"
# The Korvo-1 console is a CP2102N bridge (schematic), unlike the native
# USB-JTAG (303a:1001) of the ESP32-S3 boards that share this desk.
readonly BOARD_USB_BRIDGE="10c4:ea60"
readonly PUBLIC_BUILD_DIR="build/eidolon/korvo-1"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/${PUBLIC_BUILD_DIR}"
SDKCONFIG_FILE="${BUILD_DIR}/sdkconfig.korvo-1"
SDKCONFIG_OVERLAY="${BUILD_DIR}/sdkconfig.overlay.korvo-1"
PORT="${EIDOLON_PORT:-}"
USB_SERIAL="${EIDOLON_USB_SERIAL:-}"

# shellcheck source=/dev/null
source "${SCRIPT_DIR}/eidolon-common.sh"

die()  { echo "error: $*" >&2; exit 1; }
info() { echo ">> $*"; }

require_idf() { eidolon_require_idf "${BOARD_IDF_VERSION}"; }

# Refuse to write to anything that is not an ESP32-S31.
#
# Port selection already narrows to the Korvo-1's bridge, but EIDOLON_PORT can
# name any port by hand, so the last word belongs to the chip answering for
# itself. This opens only the one selected port.
verify_target_chip() {
  local port="$1" detected
  require_idf
  detected="$(esptool.py --port "${port}" --before default_reset --connect-attempts 2 chip_id 2>&1 |
              sed -n 's/^Detecting chip type\.\.\. //p;s/^Chip type: *\([^ ]*\).*/\1/p' | head -1)"
  if [[ -z "${detected}" ]]; then
    die "could not identify the chip on ${port}; refusing to flash blind"
  fi
  # ${var,,} is bash 4; macOS ships bash 3.2, so lowercase with tr instead.
  local normalized
  normalized="$(printf '%s' "${detected//-/}" | tr '[:upper:]' '[:lower:]')"
  if [[ "${normalized}" != "esp32s31" ]]; then
    die "${port} reports '${detected}', not ESP32-S31. Refusing to write — that is another board."
  fi
  info "Confirmed ESP32-S31 on ${port}"
}

# EIDOLON_PORT wins. Otherwise only ports behind the Korvo-1's bridge are
# candidates, never "whatever serial port is plugged in". Callers capture this
# function's stdout as the port, so everything else it runs speaks on stderr.
detect_port() {
  if [[ -n "${PORT}" ]]; then
    echo "${PORT}"
    return 0
  fi
  require_idf >&2
  local -a ports=()
  while IFS= read -r line; do
    [[ -n "${line}" ]] && ports+=("${line}")
  done < <(eidolon_usb_ports "${BOARD_USB_BRIDGE}" "${USB_SERIAL}")

  if ((${#ports[@]} == 0)); then
    die "no Korvo-1 console (USB ${BOARD_USB_BRIDGE}${USB_SERIAL:+, serial ${USB_SERIAL}}) is plugged in; see '$0 list-ports'"
  fi
  if ((${#ports[@]} > 1)); then
    printf 'More than one Korvo-1 is plugged in; set EIDOLON_USB_SERIAL or EIDOLON_PORT:\n' >&2
    eidolon_usb_port_table "${BOARD_USB_BRIDGE}" | grep '^\*' >&2
    exit 1
  fi
  echo "${ports[0]}"
}

# Board overlay from config.json, and a fresh sdkconfig whenever any of the
# defaults it was generated from changed.
prepare_config() {
  eidolon_board_overlay "${PROJECT_ROOT}" "${BOARD_PATH}" "${BOARD_KCONFIG}" "${SDKCONFIG_OVERLAY}"
  eidolon_sdkconfig_track "${SDKCONFIG_FILE}" "${config_defaults[@]}"
}

config_defaults=(
  "${PROJECT_ROOT}/sdkconfig.defaults"
  "${PROJECT_ROOT}/sdkconfig.defaults.${BOARD_TARGET}"
  "${SDKCONFIG_OVERLAY}"
)

idf_args() {
  local joined="" file
  for file in "${config_defaults[@]}"; do
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
  prepare_config
  local -a base_args=()
  while IFS= read -r arg; do base_args+=("${arg}"); done < <(idf_args)
  (cd "${PROJECT_ROOT}" && idf.py "${base_args[@]}" "$@")
}

usage() {
  cat <<EOF
Usage: $0 <command>

Commands:
  build            Build ESP32-S31-Korvo-1 Eidolon firmware (ESP-IDF v${BOARD_IDF_VERSION})
  flash [--app-only]
                   Build, verify the chip is an ESP32-S31, flash, then check the
                   boot build stamp. --app-only preserves bootloader and assets
  monitor          Open idf.py monitor
  verify           Read the boot build stamp over serial and diff vs the last build
  logs [SECONDS] [noreset]
                   Capture the serial console (default 30 s, resetting the board
                   first) to ${BUILD_DIR}/logs/ and the terminal. On this board
                   the CP2102N resets the chip whenever the port opens, so
                   noreset only skips the extra pulse
  erase-flash --yes
                   Erase the whole chip (Wi-Fi, device identity, claim, Owner
                   trust). The documented way onto a new partition layout; the
                   board must be provisioned and approved again afterwards
  partitions       Read the board's partition table and compare it with the
                   build's (read-only; what to check when flash refuses)
  size             App size against the partition, by memory region
  size-components  App size by archive (what to trim when the partition is tight)
  clean            Remove ${BUILD_DIR}
  list-ports       USB serial ports; '*' marks Korvo-1 consoles (${BOARD_USB_BRIDGE})

Environment:
  EIDOLON_PORT=/dev/cu.usbserial-XXXX
  EIDOLON_USB_SERIAL=<CP2102N serial number>
  EIDOLON_IDF_VERSION=${BOARD_IDF_VERSION}
EOF
}

if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
  return 0
fi

cmd="${1:-build}"
case "${cmd}" in
  build)
    eidolon_prepare_build "${PROJECT_ROOT}" "${BOARD_IDF_VERSION}"
    run_idf build
    ;;
  flash)
    [[ $# -le 2 && ( $# -eq 1 || "${2}" == "--app-only" ) ]] || die "Usage: $0 flash [--app-only]"
    require_idf
    PORT="$(detect_port)"
    info "Using serial port: ${PORT}"
    verify_target_chip "${PORT}"
    eidolon_prepare_build "${PROJECT_ROOT}" "${BOARD_IDF_VERSION}"
    if [[ "${2:-}" == "--app-only" ]]; then
      eidolon_flash "${PROJECT_ROOT}" "${BUILD_DIR}" "${PORT}" app-flash run_idf -p "${PORT}"
    else
      eidolon_flash "${PROJECT_ROOT}" "${BUILD_DIR}" "${PORT}" flash run_idf -p "${PORT}"
    fi
    ;;
  monitor)
    require_idf
    PORT="$(detect_port)"
    info "Using serial port: ${PORT}"
    run_idf -p "${PORT}" monitor
    ;;
  verify)
    require_idf
    PORT="$(detect_port)"
    eidolon_verify_flashed "${PROJECT_ROOT}" "${PORT}"
    ;;
  logs)
    require_idf
    PORT="$(detect_port)"
    info "Capturing ${PORT} for ${2:-30}s"
    eidolon_serial_capture "${PORT}" "${2:-30}" "$([[ "${3:-}" == noreset ]] && echo n || echo y)" \
      "${BUILD_DIR}/logs/$(date +%Y%m%d-%H%M%S).log"
    ;;
  erase-flash)
    # No script erases Owner data on its own (scripts/eidolon/README.md); this
    # runs only when asked in so many words, and only on a confirmed ESP32-S31.
    [[ "${2:-}" == "--yes" ]] ||
      die "erase-flash wipes Wi-Fi, device identity, claim and Owner trust; re-run as: $0 erase-flash --yes"
    require_idf
    PORT="$(detect_port)"
    verify_target_chip "${PORT}"
    info "Erasing the entire flash on ${PORT}"
    esptool.py --chip "${BOARD_TARGET}" --port "${PORT}" erase-flash
    ;;
  partitions)
    require_idf
    PORT="$(detect_port)"
    "$(eidolon_idf_python)" "${PROJECT_ROOT}/scripts/eidolon/partition_contract.py" diff "${BUILD_DIR}" --port "${PORT}"
    ;;
  size)            run_idf size ;;
  size-components) run_idf size-components ;;
  clean)           rm -rf "${BUILD_DIR}" ;;
  list-ports)
    require_idf
    eidolon_usb_port_table "${BOARD_USB_BRIDGE}"
    ;;
  -h|--help|help) usage ;;
  *) usage >&2; exit 2 ;;
esac
