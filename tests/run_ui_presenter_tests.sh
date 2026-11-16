#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output="$(mktemp "${TMPDIR:-/tmp}/eidolon-ui-presenter.XXXXXX")"
trap 'rm -f "$output"' EXIT
# HALF_DUPLEX is StackChan's mode. Touch and the session button are declared so
# a start or end can be driven the way a person makes it.
for interaction in FULL_DUPLEX HALF_DUPLEX PTT; do
"${CXX:-c++}" -DCONFIG_EIDOLON_INTERACTION_MODE_${interaction}=1 \
  -DCONFIG_EIDOLON_UI_TOUCH_CONTROLS=1 -DCONFIG_EIDOLON_UI_SESSION_BUTTON=1 \
  -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I tests/ui_presenter/stubs -I main tests/ui_presenter_test.cc \
  main/eidolon/eidolon_ui_presenter.cc main/eidolon/ui_state_mapper.cc \
  main/eidolon/eidolon_ui_labels.cc main/eidolon/voice_runtime_projector.cc \
  main/eidolon/agent_session_tracker.cc main/eidolon/eidolon_view.cc -o "$output"
"$output"

done
