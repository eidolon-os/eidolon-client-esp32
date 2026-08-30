#pragma once

// mbedtls 3 (ESP-IDF 5) vs mbedtls 4 (ESP-IDF 6).
//
// mbedtls 4 made PSA Crypto the public interface and moved the legacy low-level
// headers under mbedtls/private/, where the declarations are further gated
// behind MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS. The implementations, signatures
// and semantics are unchanged — only where they are declared. Selecting the
// location once here keeps every call site free of version checks and leaves
// the crypto code itself untouched.
//
// Headers that stayed public in mbedtls 4 (base64, md, pk, x509_crt, asn1, oid,
// private_access) are deliberately absent: files include those directly.
//
// This exists to survive a relocation, not to endorse it — MBEDTLS_PRIVATE
// identifiers are explicitly not a stable interface. The exit is a port to PSA
// Crypto, at which point this header and its includers disappear together.

#if __has_include(<mbedtls/sha256.h>)
#  define EIDOLON_MBEDTLS_LEGACY_PUBLIC 1
#else
#  define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#  define EIDOLON_MBEDTLS_LEGACY_PUBLIC 0
#endif

#if EIDOLON_MBEDTLS_LEGACY_PUBLIC
#  include <mbedtls/aes.h>
#  include <mbedtls/ctr_drbg.h>
#  include <mbedtls/ecdh.h>
#  include <mbedtls/ecdsa.h>
#  include <mbedtls/entropy.h>
#  include <mbedtls/gcm.h>
#  include <mbedtls/sha256.h>
#else
#  include <mbedtls/private/aes.h>
#  include <mbedtls/private/ctr_drbg.h>
#  include <mbedtls/private/ecdsa.h>
#  include <mbedtls/private/entropy.h>
#  include <mbedtls/private/gcm.h>
#  include <mbedtls/private/sha256.h>
// pk keeps a public header, but MBEDTLS_PK_ECKEY, mbedtls_pk_setup and
// mbedtls_pk_info_from_type moved into this one.
#  include <mbedtls/private/pk_private.h>
// No mbedtls/private/ecdh.h: mbedtls 4 removed mbedtls_ecdh_compute_shared
// outright rather than relocating it, so the one caller guards on
// EIDOLON_MBEDTLS_LEGACY_PUBLIC instead of including anything here.
#endif

#include <esp_random.h>
// pk.h stayed public in both versions; the wrappers below need its types
// regardless of which branch supplied the rest.
#include <mbedtls/md.h>
#include <mbedtls/pk.h>

// ESP-IDF 5 shipped mbedtls_esp_random in components/mbedtls/port/esp_hardware.c
// as exactly esp_fill_random() plus "return 0". ESP-IDF 6 still declares it in
// mbedtls/esp_mbedtls_random.h but no longer defines it anywhere, so the symbol
// does not link. Reproducing those three lines here is byte-for-byte the old
// behaviour on both toolchains, and needs no version branch.
static inline int eidolon_mbedtls_random(void* ctx, unsigned char* buf, size_t len)
{
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

// mbedtls 4 has no standalone entropy module, so mbedtls_entropy_free has no
// counterpart there. The context variable stays declared either way; only the
// teardown is version-specific.
#if EIDOLON_MBEDTLS_LEGACY_PUBLIC
#  define EIDOLON_ENTROPY_FREE(ctx) mbedtls_entropy_free(ctx)
#else
#  define EIDOLON_ENTROPY_FREE(ctx) ((void)(ctx))
#endif

// mbedtls 4 dropped the RNG parameters from these two: randomness now comes from
// PSA internally rather than from a caller-supplied callback. The calls are
// otherwise identical, so the difference lives here instead of at each site.
static inline int eidolon_pk_parse_key(mbedtls_pk_context* ctx,
                                       const unsigned char* key, size_t keylen,
                                       const unsigned char* pwd, size_t pwdlen,
                                       int (*f_rng)(void*, unsigned char*, size_t),
                                       void* p_rng)
{
#if EIDOLON_MBEDTLS_LEGACY_PUBLIC
    return mbedtls_pk_parse_key(ctx, key, keylen, pwd, pwdlen, f_rng, p_rng);
#else
    (void)f_rng;
    (void)p_rng;
    return mbedtls_pk_parse_key(ctx, key, keylen, pwd, pwdlen);
#endif
}

static inline int eidolon_pk_sign(mbedtls_pk_context* ctx, mbedtls_md_type_t md_alg,
                                  const unsigned char* hash, size_t hash_len,
                                  unsigned char* sig, size_t sig_size, size_t* sig_len,
                                  int (*f_rng)(void*, unsigned char*, size_t),
                                  void* p_rng)
{
#if EIDOLON_MBEDTLS_LEGACY_PUBLIC
    return mbedtls_pk_sign(ctx, md_alg, hash, hash_len, sig, sig_size, sig_len, f_rng, p_rng);
#else
    (void)f_rng;
    (void)p_rng;
    return mbedtls_pk_sign(ctx, md_alg, hash, hash_len, sig, sig_size, sig_len);
#endif
}
