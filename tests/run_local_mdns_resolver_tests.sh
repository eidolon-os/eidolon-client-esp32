#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output="$(mktemp "${TMPDIR:-/tmp}/eidolon_resolver.XXXXXX")"
trap 'rm -f "$output"' EXIT
for ipv6 in 0 1; do
  "${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    -DCONFIG_EIDOLON_HUB_MODE=1 -DCONFIG_EIDOLON_MDNS_QUERY_TIMEOUT_MS=8000 \
    -DLWIP_IPV4=1 -DLWIP_IPV6="$ipv6" \
    -I tests/resolver_stubs -I tests/stubs -I main \
    tests/local_mdns_resolver_test.cc main/eidolon/local_mdns_resolver.cc -o "$output"
  "$output"
done
# Non-Hub configurations keep the ordinary lwIP resolver.
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -DCONFIG_EIDOLON_HUB_MODE=0 \
  -I tests/resolver_stubs -I tests/stubs -c main/eidolon/local_mdns_resolver.cc -o "$output"
