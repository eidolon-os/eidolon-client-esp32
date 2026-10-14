#!/bin/bash
# Build only. Requires a disposable detached worktree; never flashes hardware.
set -euo pipefail
stutter_tools_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[[ $# == 2 && ( "$2" == 0 || "$2" == 150 ) ]] || {
  echo 'Usage: build_variant.sh DETACHED_WORKTREE 0|150' >&2; exit 2;
}
stutter_target="$(cd "$1" && pwd)"
stutter_threshold="$2"
cd "$stutter_target"
if git symbolic-ref -q HEAD >/dev/null; then
  echo 'Use an isolated detached worktree, not a working branch.' >&2; exit 2
fi
if git apply --check "$stutter_tools_dir/heap-diagnostics.patch"; then
  git apply "$stutter_tools_dir/heap-diagnostics.patch"
else
  git apply --reverse --check "$stutter_tools_dir/heap-diagnostics.patch"
fi
export EIDOLON_AUDIO_RX_DIAG=y
source scripts/eidolon/eidolon-esp-box-3.sh
require_idf
write_overlay
ensure_box3_sdkconfig
set_sdkconfig_bool EIDOLON_AUTO_JOIN_ON_ACTIVATION y
set_sdkconfig_value EIDOLON_LIVEKIT_PLAYOUT_BUFFER_MS "$stutter_threshold"
base_args=()
while IFS= read -r arg; do base_args+=("$arg"); done < <(idf_args)
idf.py "${base_args[@]}" build
