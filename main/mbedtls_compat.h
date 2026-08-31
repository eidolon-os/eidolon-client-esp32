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
// outright rather than relocating it. PSA key agreement replaces it below.
#  include <psa/crypto.h>
#endif

#include <string.h>

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

// ---------------------------------------------------------------------------
// Operations mbedtls 4 removed rather than relocated.
//
// Each one reaches the same result through the PSA interface that replaced it.
// The mbedtls 3 branches are the previous call-site code moved here unchanged,
// so the toolchain that ships on the esp32s3 boards keeps running exactly the
// instructions it ran before.
// ---------------------------------------------------------------------------

// Generate the device's P-256 keypair into an empty pk context. The caller then
// serialises it with mbedtls_pk_write_key_pem and stores the PEM in NVS, so the
// key has to leave the PSA key store — hence PSA_KEY_USAGE_EXPORT. Losing this
// key puts the device into a Hub 401 loop, so the two paths must produce the
// same thing: a SECP256R1 pair whose PEM parses back and whose SPKI DER hashes
// to the same device_instance_id.
static inline int eidolon_pk_gen_p256(mbedtls_pk_context* pk,
                                      mbedtls_ctr_drbg_context* drbg)
{
#if EIDOLON_MBEDTLS_LEGACY_PUBLIC
    int ret = mbedtls_pk_setup(pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (ret != 0) {
        return ret;
    }
    return mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(*pk),
                               mbedtls_ctr_drbg_random, drbg);
#else
    // PSA draws from the same hardware RNG the DRBG is seeded from, so the
    // caller's DRBG has nothing left to contribute here.
    (void)drbg;
    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        return (int)status;
    }
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_SIGN_HASH);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    mbedtls_svc_key_id_t key_id = MBEDTLS_SVC_KEY_ID_INIT;
    status = psa_generate_key(&attributes, &key_id);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS) {
        return (int)status;
    }
    // The copy is independent of the PSA key, which is destroyed straight after:
    // nothing should outlive this call inside the key store.
    int ret = mbedtls_pk_copy_from_psa(key_id, pk);
    psa_destroy_key(key_id);
    return ret;
#endif
}

// ECDSA signatures arrive as raw r||s on the wire; mbedtls_pk_verify wants DER.
static inline int eidolon_ecdsa_raw_to_der(const unsigned char raw[64],
                                           unsigned char* der, size_t der_size,
                                           size_t* der_len)
{
    unsigned char body[72];
    size_t body_len = 0;
    for (int half = 0; half < 2; ++half) {
        const unsigned char* value = raw + half * 32;
        size_t offset = 0;
        // DER integers carry no leading zero bytes, but never shrink to nothing.
        while (offset < 31 && value[offset] == 0) {
            ++offset;
        }
        const size_t length = 32 - offset;
        // A leading bit of 1 would read as a negative integer without a pad byte.
        const size_t pad = (value[offset] & 0x80) ? 1 : 0;
        body[body_len++] = 0x02;
        body[body_len++] = (unsigned char)(length + pad);
        if (pad) {
            body[body_len++] = 0x00;
        }
        memcpy(body + body_len, value + offset, length);
        body_len += length;
    }
    if (der_size < body_len + 2) {
        return MBEDTLS_ERR_PK_BUFFER_TOO_SMALL;
    }
    der[0] = 0x30;
    der[1] = (unsigned char)body_len;
    memcpy(der + 2, body, body_len);
    *der_len = body_len + 2;
    return 0;
}

// Verify a raw r||s P-256 signature over an already computed SHA-256 digest.
static inline int eidolon_pk_verify_p256_raw(mbedtls_pk_context* pk,
                                             const unsigned char* digest,
                                             size_t digest_len,
                                             const unsigned char raw[64])
{
#if EIDOLON_MBEDTLS_LEGACY_PUBLIC
    mbedtls_mpi r;
    mbedtls_mpi s;
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    int ret = mbedtls_mpi_read_binary(&r, raw, 32);
    if (ret == 0) {
        ret = mbedtls_mpi_read_binary(&s, raw + 32, 32);
    }
    if (ret == 0) {
        const mbedtls_ecp_keypair* key = mbedtls_pk_ec(*pk);
        ret = mbedtls_ecdsa_verify(
            const_cast<mbedtls_ecp_group*>(&key->MBEDTLS_PRIVATE(grp)),
            digest, digest_len, &key->MBEDTLS_PRIVATE(Q), &r, &s);
    }
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    return ret;
#else
    unsigned char der[72];
    size_t der_len = 0;
    int ret = eidolon_ecdsa_raw_to_der(raw, der, sizeof(der), &der_len);
    if (ret != 0) {
        return ret;
    }
    return mbedtls_pk_verify(pk, MBEDTLS_MD_SHA256, digest, digest_len, der, der_len);
#endif
}

// ECDH between the recipient's private key and a peer's uncompressed public
// point, returning the shared x-coordinate and the recipient's own public point
// — both of which the HPKE key schedule needs.
static inline int eidolon_ecdh_p256(mbedtls_pk_context* key,
                                    const unsigned char* peer, size_t peer_len,
                                    unsigned char shared[32],
                                    unsigned char point[65], size_t* point_len)
{
#if EIDOLON_MBEDTLS_LEGACY_PUBLIC
    mbedtls_ecp_keypair* pair = mbedtls_pk_ec(*key);
    mbedtls_ecp_point ephemeral;
    mbedtls_ecp_point_init(&ephemeral);
    mbedtls_mpi shared_mpi;
    mbedtls_mpi_init(&shared_mpi);
    int ret = mbedtls_ecp_point_read_binary(
        &pair->MBEDTLS_PRIVATE(grp), &ephemeral, peer, peer_len);
    if (ret == 0) {
        ret = mbedtls_ecdh_compute_shared(
            &pair->MBEDTLS_PRIVATE(grp), &shared_mpi, &ephemeral,
            &pair->MBEDTLS_PRIVATE(d), eidolon_mbedtls_random, nullptr);
    }
    if (ret == 0) {
        ret = mbedtls_mpi_write_binary(&shared_mpi, shared, 32);
    }
    if (ret == 0) {
        ret = mbedtls_ecp_point_write_binary(
            &pair->MBEDTLS_PRIVATE(grp), &pair->MBEDTLS_PRIVATE(Q),
            MBEDTLS_ECP_PF_UNCOMPRESSED, point_len, point, 65);
    }
    mbedtls_mpi_free(&shared_mpi);
    mbedtls_ecp_point_free(&ephemeral);
    return ret;
#else
    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        return (int)status;
    }
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    int ret = mbedtls_pk_get_psa_attributes(key, PSA_KEY_USAGE_DERIVE, &attributes);
    if (ret != 0) {
        psa_reset_key_attributes(&attributes);
        return ret;
    }
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDH);
    mbedtls_svc_key_id_t key_id = MBEDTLS_SVC_KEY_ID_INIT;
    ret = mbedtls_pk_import_into_psa(key, &attributes, &key_id);
    psa_reset_key_attributes(&attributes);
    if (ret != 0) {
        return ret;
    }
    // psa_raw_key_agreement returns the x-coordinate, which is what
    // mbedtls_ecdh_compute_shared produced as well.
    size_t shared_len = 0;
    status = psa_raw_key_agreement(PSA_ALG_ECDH, key_id, peer, peer_len,
                                   shared, 32, &shared_len);
    psa_destroy_key(key_id);
    if (status != PSA_SUCCESS) {
        return (int)status;
    }
    if (shared_len != 32) {
        return MBEDTLS_ERR_PK_BAD_INPUT_DATA;
    }
    // PSA's public key export format for an EC key is the uncompressed point,
    // the same 65 bytes mbedtls_ecp_point_write_binary produced.
    return mbedtls_pk_write_pubkey_psa(key, point, 65, point_len);
#endif
}
