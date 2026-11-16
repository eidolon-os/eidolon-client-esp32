#!/usr/bin/env bash
# The vendored LiveKit SDK's outgoing data stream, encoded and decoded with the
# real nanopb and the SDK's own generated bindings: header attributes, the
# participants every packet is addressed to, and that a header carrying
# attributes still decodes on the receive path (transcription streams send them).
set -euo pipefail
cd "$(dirname "$0")/.."
sdk="${EIDOLON_LIVEKIT_SDK:-../vendor/client-sdk-esp32}/components/livekit"
nanopb="${EIDOLON_NANOPB:-managed_components/livekit__nanopb}"
if [[ ! -d "$nanopb/src" ]]; then
  nanopb="$(git rev-parse --path-format=absolute --git-common-dir)/../managed_components/livekit__nanopb"
fi
[[ -d "$sdk/core" && -d "$nanopb/src" ]] || { echo "LiveKit SDK or nanopb sources not found" >&2; exit 1; }
output="$(mktemp "${TMPDIR:-/tmp}/eidolon-livekit-writer.XXXXXX")"
trap 'rm -f "$output"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined \
  -DPB_ENABLE_MALLOC=1 -DPB_BUFFER_ONLY=1 -DPB_VALIDATE_UTF8=1 -DCONFIG_LK_MAX_DATA_STREAM_WRITERS=2 \
  -I tests/livekit_writer_stubs -I "$sdk/include" -I "$sdk/core" -I "$sdk/protocol" -I "$nanopb/include" \
  tests/livekit_data_stream_writer_test.c "$sdk/core/data_stream_writer.c" \
  "$sdk/protocol/livekit_models.pb.c" "$sdk/protocol/livekit_rtc.pb.c" "$sdk/protocol/livekit_metrics.pb.c" \
  "$sdk/protocol/timestamp.pb.c" "$nanopb/src/pb_common.c" "$nanopb/src/pb_encode.c" "$nanopb/src/pb_decode.c" \
  -o "$output"
"$output"
