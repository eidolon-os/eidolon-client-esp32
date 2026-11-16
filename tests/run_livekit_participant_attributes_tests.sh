#!/usr/bin/env bash
# The vendored LiveKit SDK decodes participant attributes from signalling (the
# Owner policy revision rides on one) and frees them with the response.
set -euo pipefail
cd "$(dirname "$0")/.."
sdk="${EIDOLON_LIVEKIT_SDK:-../vendor/client-sdk-esp32}/components/livekit"
nanopb="${EIDOLON_NANOPB:-managed_components/livekit__nanopb}"
if [[ ! -d "$nanopb/src" ]]; then
  nanopb="$(git rev-parse --path-format=absolute --git-common-dir)/../managed_components/livekit__nanopb"
fi
[[ -d "$sdk/protocol" && -d "$nanopb/src" ]] || { echo "LiveKit SDK or nanopb sources not found" >&2; exit 1; }
output="$(mktemp "${TMPDIR:-/tmp}/eidolon-livekit-attributes.XXXXXX")"
trap 'rm -f "$output"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined \
  -DPB_ENABLE_MALLOC=1 -DPB_BUFFER_ONLY=1 -DPB_VALIDATE_UTF8=1 \
  -include tests/livekit_attribute_stubs/counting_alloc.h \
  -I "$sdk/protocol" -I "$nanopb/include" \
  tests/livekit_participant_attributes_test.c \
  "$sdk/protocol/livekit_models.pb.c" "$sdk/protocol/livekit_rtc.pb.c" "$sdk/protocol/livekit_metrics.pb.c" \
  "$sdk/protocol/timestamp.pb.c" "$nanopb/src/pb_common.c" "$nanopb/src/pb_encode.c" "$nanopb/src/pb_decode.c" \
  -o "$output"
"$output"
