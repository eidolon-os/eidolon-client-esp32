#!/usr/bin/env bash
# 兼容入口 -> 板型专用脚本
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/eidolon-esp32-s3-touch-amoled-2.06.sh" "$@"
