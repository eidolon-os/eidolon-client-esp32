#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output="$(mktemp "${TMPDIR:-/tmp}/eidolon-ui-presenter.XXXXXX")"
trap 'rm -f "$output"' EXIT
for interaction in FULL_DUPLEX PTT; do
"${CXX:-c++}" -DCONFIG_EIDOLON_INTERACTION_MODE_${interaction}=1 -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I tests/ui_presenter/stubs -I main tests/ui_presenter_test.cc \
  main/eidolon/eidolon_ui_presenter.cc main/eidolon/ui_state_mapper.cc \
  main/eidolon/eidolon_ui_labels.cc main/eidolon/voice_runtime_projector.cc \
  main/eidolon/agent_session_tracker.cc main/eidolon/eidolon_view.cc -o "$output"
"$output"

done
