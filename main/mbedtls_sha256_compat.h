#pragma once

// mbedTLS 3 exposes SHA-256 publicly; mbedTLS 4 relocates the same API under
// mbedtls/private. Keep that version boundary narrow so code which only hashes
// data does not acquire every key-generation and cipher declaration from the
// full compatibility layer.

#if __has_include(<mbedtls/sha256.h>)
#  define EIDOLON_MBEDTLS_LEGACY_PUBLIC 1
#  include <mbedtls/sha256.h>
#else
#  define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#  define EIDOLON_MBEDTLS_LEGACY_PUBLIC 0
#  include <mbedtls/private/sha256.h>
#endif
