#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/eidolon-http-tests.XXXXXX")"
trap 'rm -rf "$build_dir"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I main \
    tests/commissioning_http_socket_policy_test.cc -o "$build_dir/socket-policy"
"$build_dir/socket-policy"
: "${IDF_PATH:?Set IDF_PATH to the firmware SDK for its actual lwIP sources}"
lwip_src="$IDF_PATH/components/lwip/lwip/src"
"${CC:-cc}" -I tests/provisioning_lwip -I "$lwip_src/include" \
    tests/provisioning_lwip/probe.c "$lwip_src"/core/*.c "$lwip_src"/core/ipv4/*.c \
    -o "$build_dir/lwip-framing"
"$build_dir/lwip-framing"
