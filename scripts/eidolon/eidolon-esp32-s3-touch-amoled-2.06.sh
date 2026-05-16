#!/usr/bin/env bash
# Eidolon dev toolkit — Waveshare ESP32-S3-Touch-AMOLED-2.06
#
# 交互入口（无参数，启动时自动配置板型）:
#   ./scripts/eidolon/eidolon-esp32-s3-touch-amoled-2.06.sh
#
# 命令行模式:
#   ./scripts/eidolon/eidolon-esp32-s3-touch-amoled-2.06.sh build
#   ./scripts/eidolon/eidolon-esp32-s3-touch-amoled-2.06.sh flash
#
# 兼容入口:
#   ./scripts/eidolon/eidolon.sh  -> 转发到本脚本
#
# Environment:
#   EIDOLON_PORT      默认串口
#   EIDOLON_IDF_PATH  ESP-IDF 根目录（优先于自动查找）
#   IDF_PATH          同上，官方变量名
#
# 可选：在本目录创建 idf.path，写入一行 ESP-IDF 绝对路径
#   echo ~/esp/esp-idf > scripts/eidolon/idf.path

set -euo pipefail

# ---------------------------------------------------------------------------
# Board defaults
# ---------------------------------------------------------------------------
readonly BOARD_PATH="waveshare/esp32-s3-touch-amoled-2.06"
readonly BOARD_NAME="esp32-s3-touch-amoled-2.06"
readonly BOARD_TARGET="esp32s3"
readonly BOARD_KCONFIG="CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_AMOLED_2_06=y"
readonly PARTITION_CSV="partitions/v2/16m.csv"
readonly FLASH_SIZE="16MB"
readonly MONITOR_BAUD="115200"
readonly SDK_MARKER="# Append by eidolon-esp32-s3-touch-amoled-2.06.sh"
readonly SCRIPT_LABEL="eidolon-esp32-s3-touch-amoled-2.06"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

INTERACTIVE=0
BOARD_AUTO_CONFIGURED=0
IDF_EXPORT_FILE=""
PORT="${EIDOLON_PORT:-}"
BAUD="${MONITOR_BAUD}"
CLEAN_FIRST=0
SKIP_BUILD=0
APP_ONLY=0
ERASE_ALL=0
declare -a ERASE_PARTITIONS=()
declare -a ERASE_GROUPS=()
declare -a FLASH_PARTITIONS=()

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
die() {
  echo "error: $*" >&2
  exit 1
}

info() {
  echo ">> $*"
}

idf_ready() {
  command -v idf.py >/dev/null 2>&1
}

# Collect candidate export.sh paths (first match wins)
_idf_export_candidates() {
  local cfg="${SCRIPT_DIR}/idf.path"
  if [[ -f "${cfg}" ]]; then
    local line
    while IFS= read -r line || [[ -n "${line}" ]]; do
      line="${line%%#*}"
      line="${line#"${line%%[![:space:]]*}"}"
      line="${line%"${line##*[![:space:]]}"}"
      [[ -n "${line}" ]] && echo "${line%/}/export.sh"
    done <"${cfg}"
  fi

  local root=""
  for root in "${EIDOLON_IDF_PATH:-}" "${IDF_PATH:-}"; do
    [[ -n "${root}" ]] && echo "${root%/}/export.sh"
  done

  local home="${HOME:-}"
  if [[ -n "${home}" ]]; then
    echo "${home}/esp/esp-idf/export.sh"
    echo "${home}/esp-idf/export.sh"
    local d
    shopt -s nullglob
    for d in "${home}"/esp/*/export.sh "${home}"/esp-idf-*/export.sh; do
      echo "${d}"
    done
    shopt -u nullglob
  fi
}

find_idf_export() {
  local f
  while IFS= read -r f; do
    [[ -f "${f}" ]] || continue
    echo "${f}"
    return 0
  done < <(_idf_export_candidates | awk '!seen[$0]++')
  return 1
}

# Auto source ESP-IDF export.sh in current shell
ensure_idf_env() {
  if idf_ready; then
    return 0
  fi

  local export_sh=""
  if ! export_sh="$(find_idf_export)"; then
    return 1
  fi

  echo ">> 加载 ESP-IDF: ${export_sh}"
  # shellcheck source=/dev/null
  source "${export_sh}"
  IDF_EXPORT_FILE="${export_sh}"

  idf_ready
}

require_idf() {
  if ensure_idf_env; then
    return 0
  fi
  die "未找到 ESP-IDF。请设置 IDF_PATH 或在本目录创建 idf.path 写入 IDF 根路径"
}

menu_sep() {
  printf "  ---------- %s ----------\n" "$1"
}

run() {
  info "$*"
  (cd "${PROJECT_ROOT}" && "$@")
}

idf() {
  local args=("idf.py" "$@")
  [[ -n "${PORT}" ]] && args+=("-p" "${PORT}")
  run "${args[@]}"
}

reset_task_flags() {
  CLEAN_FIRST=0
  SKIP_BUILD=0
  APP_ONLY=0
  ERASE_ALL=0
  ERASE_PARTITIONS=()
  ERASE_GROUPS=()
  FLASH_PARTITIONS=()
}

pause_menu() {
  echo ""
  read -r -p "按回车键返回主菜单..." _
}

# ---------------------------------------------------------------------------
# Serial port
# ---------------------------------------------------------------------------
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
    [[ -e "${p}" ]] || continue
    if [[ "${p}" == /dev/tty.* ]]; then
      local cu="/dev/cu.${p#/dev/tty.}"
      [[ -e "${cu}" ]] && continue
    fi
    echo "${p}"
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
    [[ "${INTERACTIVE}" -eq 1 ]] && return 1
    die "no USB serial port found. Set EIDOLON_PORT=/dev/cu.xxx"
  fi

  if ((${#ports[@]} == 1)); then
    echo "${ports[0]}"
    return 0
  fi

  local preferred="" p
  for p in "${ports[@]}"; do
    if [[ "${p}" == *usbmodem* || "${p}" == *wchusbserial* ]]; then
      preferred="${p}"
      break
    fi
  done
  [[ -z "${preferred}" ]] && preferred="${ports[0]}"

  echo "available ports:" >&2
  for p in "${ports[@]}"; do
    if [[ "${p}" == "${preferred}" ]]; then
      echo "  ${p}  <-- selected" >&2
    else
      echo "  ${p}" >&2
    fi
  done
  echo "${preferred}"
}

menu_pick_port() {
  local -a ports=()
  while IFS= read -r line; do
    [[ -n "${line}" ]] && ports+=("${line}")
  done < <(list_ports | sort -u)

  echo ""
  echo "串口选择:"
  if ((${#ports[@]} == 0)); then
    echo "  (未检测到 USB 串口)"
    read -r -p "手动输入串口路径 (留空取消): " manual
    [[ -n "${manual}" ]] && PORT="${manual}"
    return
  fi

  local i=1 p
  for p in "${ports[@]}"; do
    echo "  [${i}] ${p}"
    ((i++)) || true
  done
  echo "  [0] 自动检测 (清除手动设置)"
  echo "  当前: ${PORT:-自动}"
  read -r -p "请选择 [0-${#ports[@]}]: " choice

  if [[ -z "${choice}" || "${choice}" == "0" ]]; then
    PORT="${EIDOLON_PORT:-}"
    echo "已设为自动检测"
    return
  fi

  if [[ "${choice}" =~ ^[0-9]+$ ]] && ((choice >= 1 && choice <= ${#ports[@]})); then
    PORT="${ports[$((choice - 1))]}"
    echo "已选择: ${PORT}"
    return
  fi

  # treat as path
  PORT="${choice}"
  echo "已选择: ${PORT}"
}

# ---------------------------------------------------------------------------
# Partition table (v2 / 16MB)
# ---------------------------------------------------------------------------
partition_offset() {
  case "$1" in
    nvs)      echo "0x9000" ;;
    otadata)  echo "0xd000" ;;
    phy_init) echo "0xf000" ;;
    ota_0)    echo "0x20000" ;;
    ota_1)    echo "0x410000" ;;
    assets)   echo "0x800000" ;;
    *) die "unknown partition: $1" ;;
  esac
}

partition_size() {
  case "$1" in
    nvs)      echo "0x4000" ;;
    otadata)  echo "0x2000" ;;
    phy_init) echo "0x1000" ;;
    ota_0)    echo "0x3f0000" ;;
    ota_1)    echo "0x3f0000" ;;
    assets)   echo "0x800000" ;;
    *) die "unknown partition: $1" ;;
  esac
}

expand_group() {
  case "$1" in
    config) echo "nvs otadata" ;;
    app)    echo "ota_0 ota_1" ;;
    assets) echo "assets" ;;
    data)   echo "nvs otadata phy_init assets" ;;
    all)    echo "" ;;
    *) die "unknown group: $1" ;;
  esac
}

show_partitions() {
  local built="${PROJECT_ROOT}/build/partition_table/partition-table.csv"
  echo "board   : ${BOARD_NAME}"
  echo "target  : ${BOARD_TARGET}"
  echo "flash   : ${FLASH_SIZE}"
  if [[ -f "${built}" ]]; then
    echo "layout  : ${built} (from last build)"
    cat "${built}"
  else
    echo "layout  : ${PARTITION_CSV} (default)"
    local name
    for name in nvs otadata phy_init ota_0 ota_1 assets; do
      printf "  %-10s offset=%-10s size=%s\n" \
        "${name}" "$(partition_offset "${name}")" "$(partition_size "${name}")"
    done
  fi
}

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
sdkconfig_has_target() {
  local sdkconfig="${PROJECT_ROOT}/sdkconfig"
  [[ -f "${sdkconfig}" ]] || return 1
  grep -qE '^CONFIG_IDF_TARGET_ESP32S3=y' "${sdkconfig}" 2>/dev/null
}

sdkconfig_has_board() {
  local sdkconfig="${PROJECT_ROOT}/sdkconfig"
  [[ -f "${sdkconfig}" ]] && grep -q "${BOARD_KCONFIG}" "${sdkconfig}"
}

# 取消 sdkconfig 中其它板型的 =y，避免与当前板型冲突
clear_other_board_selections() {
  local sdkconfig="${PROJECT_ROOT}/sdkconfig"
  [[ -f "${sdkconfig}" ]] || return 0

  local tmp
  tmp="$(mktemp)"
  # 注释掉其它 BOARD_TYPE 选项，保留本板型
  awk -v keep="${BOARD_KCONFIG}" '
    /^CONFIG_BOARD_TYPE_[A-Z0-9_]+=y/ {
      if ($0 != keep) { print "# " $0 " (disabled by eidolon)"; next }
    }
    { print }
  ' "${sdkconfig}" >"${tmp}"
  mv "${tmp}" "${sdkconfig}"
}

ensure_board_sdkconfig() {
  local sdkconfig="${PROJECT_ROOT}/sdkconfig"
  clear_other_board_selections

  if sdkconfig_has_board; then
    return 0
  fi

  info "写入板型 sdkconfig: ${BOARD_NAME}"
  cat >>"${sdkconfig}" <<EOF

${SDK_MARKER}
${BOARD_KCONFIG}
CONFIG_USE_WECHAT_MESSAGE_STYLE=n
CONFIG_USE_DEVICE_AEC=y
CONFIG_BT_ENABLED=y
CONFIG_BT_BLUEDROID_ENABLED=y
CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y
CONFIG_BT_BLE_50_FEATURES_SUPPORTED=n
CONFIG_BT_BLE_BLUFI_ENABLE=y
CONFIG_MBEDTLS_DHM_C=y
EOF
}

configure_target() {
  require_idf
  if sdkconfig_has_target; then
    return 0
  fi
  info "set-target ${BOARD_TARGET}"
  run idf.py set-target "${BOARD_TARGET}"
}

# 启动时自动：加载 IDF + set-target + 选择本板型
auto_configure_board() {
  local quiet="${1:-0}"

  if ! ensure_idf_env; then
    [[ "${quiet}" -eq 0 ]] && echo "提示: ESP-IDF 未就绪，跳过板型自动配置" >&2
    return 1
  fi

  configure_target
  ensure_board_sdkconfig

  [[ "${quiet}" -eq 0 ]] && echo ">> 板型已自动配置: ${BOARD_NAME} (${BOARD_PATH})"
  return 0
}

clean_build_dir() {
  local build_dir="${PROJECT_ROOT}/build"
  if [[ -d "${build_dir}" ]]; then
    info "removing ${build_dir}"
    rm -rf "${build_dir}"
  else
    echo "build/ already clean"
  fi
}

cmd_build() {
  require_idf
  [[ "${CLEAN_FIRST}" -eq 1 ]] && clean_build_dir
  configure_target
  ensure_board_sdkconfig
  run idf.py \
    -DBOARD_NAME="${BOARD_NAME}" \
    -DBOARD_TYPE="${BOARD_PATH}" \
    build
}

# ---------------------------------------------------------------------------
# Erase / flash / monitor
# ---------------------------------------------------------------------------
erase_partition_esptool() {
  local name="$1"
  local offset size
  offset="$(partition_offset "${name}")"
  size="$(partition_size "${name}")"

  local esptool
  esptool="$(command -v esptool.py || command -v esptool || true)"
  [[ -n "${esptool}" ]] || die "esptool not found"

  info "erasing ${name}: offset=${offset} size=${size}"
  "${esptool}" --chip "${BOARD_TARGET}" -p "${PORT}" -b 460800 \
    erase_region "${offset}" "${size}"
}

erase_partitions() {
  require_idf
  if ! PORT="$(detect_port)"; then
    echo "error: 未找到串口，请先连接设备或设置串口" >&2
    return 1
  fi

  if [[ "${ERASE_ALL}" -eq 1 ]]; then
    idf erase-flash
    return
  fi

  local -a names=()
  local g
  for g in "${ERASE_GROUPS[@]}"; do
    if [[ "${g}" == "all" ]]; then
      idf erase-flash
      return
    fi
    read -r -a _parts <<<"$(expand_group "${g}")"
    names+=("${_parts[@]}")
  done
  names+=("${ERASE_PARTITIONS[@]}")

  if ((${#names[@]} == 0)); then
    die "specify --all, --group, or -p PARTITION"
  fi

  local -a unique=()
  local n seen=""
  for n in "${names[@]}"; do
    [[ " ${seen} " == *" ${n} "* ]] && continue
    seen+=" ${n}"
    unique+=("${n}")
  done

  local use_idf=0
  if idf.py erase-partition --help >/dev/null 2>&1; then
    use_idf=1
  fi

  for n in "${unique[@]}"; do
    if [[ "${use_idf}" -eq 1 ]]; then
      idf erase-partition "${n}"
    else
      erase_partition_esptool "${n}"
    fi
  done
}

cmd_flash() {
  require_idf
  if ! PORT="$(detect_port)"; then
    echo "error: 未找到串口" >&2
    return 1
  fi

  if [[ "${APP_ONLY}" -eq 1 ]]; then
    idf app-flash
    return
  fi

  if ((${#FLASH_PARTITIONS[@]} > 0)); then
    local part
    for part in "${FLASH_PARTITIONS[@]}"; do
      idf flash "--only-flash-partition=${part}"
    done
    return
  fi

  idf flash
}

cmd_monitor() {
  require_idf
  if ! PORT="$(detect_port)"; then
    echo "error: 未找到串口" >&2
    return 1
  fi
  echo "提示: 监控中按 Ctrl+] 退出串口监视器"
  idf monitor -b "${BAUD}"
}

cmd_port() {
  local p
  while IFS= read -r p; do echo "${p}"; done < <(list_ports | sort -u)
}

cmd_info() {
  show_partitions
  echo ""
  if p="$(list_ports | head -1)" && [[ -n "${p}" ]]; then
    if PORT_DETECTED="$(detect_port)"; then
      echo "detected port: ${PORT_DETECTED}"
    fi
  else
    echo "detected port: (none)"
  fi
}

cmd_setup() {
  require_idf
  info "强制重新配置板型: ${BOARD_NAME}"
  run idf.py set-target "${BOARD_TARGET}"
  ensure_board_sdkconfig
  echo ">> 板型配置已更新"
}

cmd_clean() {
  local scope="${1:-build}"
  case "${scope}" in
    build) clean_build_dir ;;
    idf)   require_idf; run idf.py fullclean || true ;;
    all)   clean_build_dir; require_idf; run idf.py fullclean || true ;;
    *) die "unknown clean scope: ${scope}" ;;
  esac
}

cmd_menuconfig() {
  require_idf
  configure_target
  ensure_board_sdkconfig
  run idf.py menuconfig
}

cmd_merge_bin() {
  require_idf
  run idf.py merge-bin
}

cmd_run() {
  [[ "${SKIP_BUILD}" -eq 0 ]] && cmd_build
  cmd_flash
  cmd_monitor
}

# ---------------------------------------------------------------------------
# CLI parsers (non-interactive)
# ---------------------------------------------------------------------------
parse_erase_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --all) ERASE_ALL=1; shift ;;
      --group) ERASE_GROUPS+=("$2"); shift 2 ;;
      -p|--partition) ERASE_PARTITIONS+=("$2"); shift 2 ;;
      -P|--port) PORT="$2"; shift 2 ;;
      *) die "unknown option: $1" ;;
    esac
  done
}

parse_flash_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --app-only) APP_ONLY=1; shift ;;
      -p|--partition) FLASH_PARTITIONS+=("$2"); shift 2 ;;
      -P|--port) PORT="$2"; shift 2 ;;
      *) die "unknown option: $1" ;;
    esac
  done
}

# ---------------------------------------------------------------------------
# Interactive menu
# ---------------------------------------------------------------------------
show_banner() {
  local idf_status port_status
  ensure_idf_env >/dev/null 2>&1 || true

  if idf_ready; then
    if [[ -n "${IDF_PATH:-}" ]]; then
      idf_status="已加载 (${IDF_PATH})"
    elif [[ -n "${IDF_EXPORT_FILE}" ]]; then
      idf_status="已加载 (${IDF_EXPORT_FILE})"
    else
      idf_status="已加载"
    fi
  else
    idf_status="未找到 (可创建 scripts/eidolon/idf.path)"
  fi

  if [[ -n "${PORT}" ]]; then
    port_status="${PORT} (手动)"
  elif p="$(list_ports | head -1)" && [[ -n "${p}" ]]; then
    port_status="${p} (自动)"
  else
    port_status="未连接"
  fi

  clear 2>/dev/null || true
  echo "============================================================"
  echo "  Eidolon 开发工具"
  echo "  硬件: Waveshare ESP32-S3-Touch-AMOLED-2.06"
  echo "============================================================"
  echo "  项目 : ${PROJECT_ROOT}"
  echo "  板型 : ${BOARD_NAME}"
  echo "  IDF  : ${idf_status}"
  echo "  串口 : ${port_status}"
  echo "============================================================"
  echo ""
  menu_sep "信息"
  echo "  [1]  查看 USB 串口列表"
  echo "  [2]  查看板子 / 分区信息"
  echo "  [3]  重新应用板型配置 (set-target + sdkconfig)"
  echo ""
  menu_sep "编译 / 烧录"
  echo "  [4]  编译固件"
  echo "  [5]  清理 build 后编译"
  echo "  [6]  烧录固件 (完整)"
  echo "  [7]  仅烧录应用 (app-flash)"
  echo "  [8]  仅烧录 assets 分区"
  echo "  [9]  串口监控"
  echo "  [10] 一键：编译 + 烧录 + 监控"
  echo ""
  menu_sep "高级"
  echo "  [11] menuconfig"
  echo "  [12] merge-bin (合并烧录包)"
  echo ""
  menu_sep "擦除 Flash"
  echo "  [13] 擦除整片 Flash"
  echo "  [14] 擦除配置分区 (nvs + otadata)"
  echo "  [15] 擦除应用分区 (ota_0 + ota_1)"
  echo "  [16] 擦除 assets 分区"
  echo ""
  menu_sep "清理 / 设置"
  echo "  [17] 清理 build 目录"
  echo "  [18] 完全清理 (build + idf fullclean)"
  echo "  [19] 设置 / 切换串口"
  echo ""
  menu_sep "退出"
  echo "  [0]  退出程序"
  echo ""
}

run_menu_task() {
  local rc=0
  set +e
  "$@"
  rc=$?
  set -e
  return "${rc}"
}

interactive_menu() {
  INTERACTIVE=1

  if [[ "${BOARD_AUTO_CONFIGURED}" -eq 0 ]]; then
    echo "正在自动配置板型 ${BOARD_NAME} ..."
    auto_configure_board 0 || true
    BOARD_AUTO_CONFIGURED=1
    echo ""
  fi

  while true; do
    reset_task_flags
    show_banner
    read -r -p "请输入选项编号: " choice
    echo ""

    case "${choice}" in
      1)
        run_menu_task cmd_port || true
        ;;
      2)
        run_menu_task cmd_info || true
        ;;
      3)
        run_menu_task cmd_setup || true
        ;;
      4)
        run_menu_task cmd_build || true
        ;;
      5)
        CLEAN_FIRST=1
        run_menu_task cmd_build || true
        ;;
      6)
        run_menu_task cmd_flash || true
        ;;
      7)
        APP_ONLY=1
        run_menu_task cmd_flash || true
        ;;
      8)
        FLASH_PARTITIONS=(assets)
        run_menu_task cmd_flash || true
        ;;
      9)
        run_menu_task cmd_monitor || true
        ;;
      10)
        run_menu_task cmd_run || true
        ;;
      11)
        run_menu_task cmd_menuconfig || true
        ;;
      12)
        run_menu_task cmd_merge_bin || true
        ;;
      13)
        ERASE_ALL=1
        run_menu_task erase_partitions || true
        ;;
      14)
        ERASE_GROUPS=(config)
        run_menu_task erase_partitions || true
        ;;
      15)
        ERASE_GROUPS=(app)
        run_menu_task erase_partitions || true
        ;;
      16)
        ERASE_PARTITIONS=(assets)
        run_menu_task erase_partitions || true
        ;;
      17)
        run_menu_task cmd_clean build || true
        ;;
      18)
        run_menu_task cmd_clean all || true
        ;;
      19)
        menu_pick_port
        ;;
      0|q|Q|exit)
        echo "再见。"
        exit 0
        ;;
      "")
        continue
        ;;
      *)
        echo "无效选项: ${choice}"
        ;;
    esac

    pause_menu
  done
}

# ---------------------------------------------------------------------------
# CLI mode
# ---------------------------------------------------------------------------
usage() {
  cat <<EOF
Eidolon dev toolkit — Waveshare ESP32-S3-Touch-AMOLED-2.06

交互菜单（推荐）:
  $(basename "$0")
  或 ./scripts/eidolon/eidolon.sh

命令行模式:
  $(basename "$0") <command> [options]

Commands:
  port | info | setup | build | flash | monitor | run
  erase | clean | menuconfig | merge-bin

ESP-IDF:
  脚本会自动 source export.sh（IDF_PATH / idf.path / ~/esp/esp-idf）
EOF
}

dispatch_cli() {
  ensure_idf_env >/dev/null 2>&1 || true
  local cmd="${1:-}"
  shift || true
  INTERACTIVE=0

  case "${cmd}" in
    port)   cmd_port ;;
    info)   cmd_info ;;
    setup)  cmd_setup ;;
    clean)  cmd_clean "${1:-build}" ;;
    build)
      auto_configure_board 1 || true
      [[ "${1:-}" == "--clean" ]] && { CLEAN_FIRST=1; shift; }
      cmd_build
      ;;
    erase)
      auto_configure_board 1 || true
      parse_erase_args "$@"
      erase_partitions
      ;;
    flash)
      auto_configure_board 1 || true
      parse_flash_args "$@"
      cmd_flash
      ;;
    monitor)
      auto_configure_board 1 || true
      cmd_monitor
      ;;
    run)
      auto_configure_board 1 || true
      [[ "${1:-}" == "--skip-build" ]] && { SKIP_BUILD=1; shift; }
      [[ "${1:-}" == "--clean" ]] && { CLEAN_FIRST=1; shift; }
      parse_flash_args "$@"
      cmd_run
      ;;
    menuconfig)
      auto_configure_board 1 || true
      cmd_menuconfig
      ;;
    merge-bin)
      auto_configure_board 1 || true
      cmd_merge_bin
      ;;
    -h|--help|help) usage ;;
    *) die "unknown command: ${cmd}" ;;
  esac
}

main() {
  if [[ $# -eq 0 ]]; then
    if ! ensure_idf_env >/dev/null 2>&1; then
      echo "提示: 未自动找到 ESP-IDF，部分功能不可用。" >&2
      echo "      可执行: echo '\$HOME/esp/esp-idf' > ${SCRIPT_DIR}/idf.path" >&2
      echo ""
    fi
    interactive_menu
  else
    dispatch_cli "$@"
  fi
}

main "$@"
