// An allocation-free HMAC-SHA1 for esp_peer's ICE agent on ESP32-S31.
//
// THE FAULT
//
// esp_peer signs and verifies every STUN MESSAGE-INTEGRITY through one helper.
// On IDF v6 that helper is `utils_get_hmac_sha1()` in the component's own
// managed_components/espressif__esp_peer/src/dtls_srtp_v6.c:294 - a file
// compiled only for IDF >= 6, which is why no ESP32-S3 board reaches it:
//
//     if (mbedtls_md_hmac(md, key, key_len, input, input_len, output) != 0) {
//         memset(output, 0, 20);
//     }
//
// When the digest fails the MAC becomes twenty zero bytes and the caller is
// told nothing - the function returns void. The ICE agent then:
//
//   * signs its keepalive Binding Requests with zeros, so the server drops them
//     without replying; after alive_binding_retries x 6s esp_peer reports
//     "Peer not response for binding after 30000" and rebuilds the whole
//     session, signalling included, which the Host closes as DUPLICATE_IDENTITY;
//
//   * compares zeros against the server's genuine MAC on every inbound check and
//     logs "E STUN: Message Integrity does not match." every 2s, this Host's
//     keepalive interval.
//
// Confirmed on the wire: of the device's Binding Requests, every one carrying a
// real MAC was answered and every one carrying
// MESSAGE-INTEGRITY=0000000000000000000000000000000000000000 was ignored, in
// the same session, on the same 5-tuple, with the same ICE credentials. All
// inbound requests had a valid FINGERPRINT, so the packets were never at fault.
//
// WHY THE DIGEST FAILS
//
// Not memory pressure in general - when this was caught in the act, PSRAM had
// 13.9 MB free. `mbedtls_md_hmac` for SHA-1 lands on IDF's PSA SHA driver, and
// esp_sha_hash_setup() in
// components/mbedtls/port/psa_driver/esp_sha/psa_crypto_driver_esp_sha.c:141
// starts every single hash with
//
//     heap_caps_malloc(sizeof(esp_sha1_context), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
//
// and returns PSA_ERROR_INSUFFICIENT_MEMORY when it cannot be served. The
// context is only about 96 bytes, but it has to come out of internal
// DMA-capable SRAM, which is the scarce pool on this board - measured at ~30 kB
// free while the request was failing. So a STUN MAC, computed twice a second
// over a 100-byte datagram, was made to compete for the same memory as the I2S
// descriptors and the Wi-Fi DMA buffers, and quietly lost.
//
// THE FIX
//
// SHA-1 here is computed in software, from a fixed-size context on the caller's
// stack. No heap, no DMA, no PSA, no hardware engine, and therefore no failure
// path: the MAC is always correct. The cost is negligible at this duty cycle -
// two ~100-byte SHA-1 blocks per STUN message, a handful of microseconds - and
// it takes the ICE keepalive completely out of the internal-DMA contention that
// this board is short of.
//
// Reached through `-Wl,--wrap=utils_get_hmac_sha1` (see main/CMakeLists.txt)
// rather than by defining the symbol outright, because dtls_srtp_v6.c defines it
// strongly and a second definition is a link error. Only stun.c inside
// libpeer_default.a references the symbol, and dtls_srtp_v6.c never calls its own
// definition, so wrapping redirects every real call site.
//
// Scoped to esp32s31: the only board here built against IDF v6 / mbedTLS 4.x.
//
// Two things worth carrying upstream, separately from this workaround:
//   - esp_peer turning a failed digest into a silent zero MAC (utils_get_md5 in
//     the same file has the identical pattern, and it backs TURN's long-term
//     credentials);
//   - IDF's PSA SHA driver requiring an internal DMA allocation per hash, which
//     makes every digest on a memory-tight board a coin flip.

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S31

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define EIDOLON_SHA1_DIGEST_LEN 20
#define EIDOLON_SHA1_BLOCK_LEN 64

typedef struct {
    uint32_t state[5];
    uint64_t count;  // message length in bytes
    uint8_t buffer[EIDOLON_SHA1_BLOCK_LEN];
    size_t buffered;
} eidolon_sha1_ctx;

static uint32_t rol(uint32_t v, int b)
{
    return (v << b) | (v >> (32 - b));
}

static void eidolon_sha1_transform(uint32_t state[5], const uint8_t block[64])
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        const uint32_t tmp = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = tmp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;

    memset(w, 0, sizeof(w));
}

static void eidolon_sha1_init(eidolon_sha1_ctx *ctx)
{
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count = 0;
    ctx->buffered = 0;
}

static void eidolon_sha1_update(eidolon_sha1_ctx *ctx, const uint8_t *data, size_t len)
{
    ctx->count += len;
    if (ctx->buffered > 0) {
        const size_t want = EIDOLON_SHA1_BLOCK_LEN - ctx->buffered;
        const size_t take = (len < want) ? len : want;
        memcpy(ctx->buffer + ctx->buffered, data, take);
        ctx->buffered += take;
        data += take;
        len -= take;
        if (ctx->buffered < EIDOLON_SHA1_BLOCK_LEN) {
            return;
        }
        eidolon_sha1_transform(ctx->state, ctx->buffer);
        ctx->buffered = 0;
    }
    while (len >= EIDOLON_SHA1_BLOCK_LEN) {
        eidolon_sha1_transform(ctx->state, data);
        data += EIDOLON_SHA1_BLOCK_LEN;
        len -= EIDOLON_SHA1_BLOCK_LEN;
    }
    if (len > 0) {
        memcpy(ctx->buffer, data, len);
        ctx->buffered = len;
    }
}

static void eidolon_sha1_final(eidolon_sha1_ctx *ctx, uint8_t *out)
{
    const uint64_t bits = ctx->count * 8;
    static const uint8_t pad_byte = 0x80;
    eidolon_sha1_update(ctx, &pad_byte, 1);
    static const uint8_t zero = 0x00;
    while (ctx->buffered != 56) {
        eidolon_sha1_update(ctx, &zero, 1);
    }
    uint8_t len_be[8];
    for (int i = 0; i < 8; i++) {
        len_be[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    eidolon_sha1_update(ctx, len_be, 8);  // triggers the final transform

    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
    memset(ctx, 0, sizeof(*ctx));
}

// Signature matches dtls_srtp_v6.c:294 exactly. `output` is always a 20-byte
// buffer; the library never passes its size.
void __wrap_utils_get_hmac_sha1(const char *input, size_t input_len,
                                const char *key, size_t key_len,
                                unsigned char *output)
{
    if (output == NULL || input == NULL || key == NULL) {
        return;
    }

    // RFC 2104: a key longer than the block size is replaced by its own digest,
    // and anything shorter is zero-padded out to the block size.
    uint8_t block[EIDOLON_SHA1_BLOCK_LEN];
    memset(block, 0, sizeof(block));
    eidolon_sha1_ctx ctx;
    if (key_len > sizeof(block)) {
        eidolon_sha1_init(&ctx);
        eidolon_sha1_update(&ctx, (const uint8_t *)key, key_len);
        eidolon_sha1_final(&ctx, block);
    } else {
        memcpy(block, key, key_len);
    }

    for (size_t i = 0; i < sizeof(block); i++) {
        block[i] ^= 0x36;  // ipad
    }
    uint8_t inner[EIDOLON_SHA1_DIGEST_LEN];
    eidolon_sha1_init(&ctx);
    eidolon_sha1_update(&ctx, block, sizeof(block));
    eidolon_sha1_update(&ctx, (const uint8_t *)input, input_len);
    eidolon_sha1_final(&ctx, inner);

    for (size_t i = 0; i < sizeof(block); i++) {
        block[i] ^= (0x36 ^ 0x5c);  // ipad -> opad
    }
    eidolon_sha1_init(&ctx);
    eidolon_sha1_update(&ctx, block, sizeof(block));
    eidolon_sha1_update(&ctx, inner, sizeof(inner));
    eidolon_sha1_final(&ctx, output);

    // The padded key is derived from the ICE password; do not leave it on the
    // stack for the next frame to inherit.
    memset(block, 0, sizeof(block));
    memset(inner, 0, sizeof(inner));
}

#endif  // CONFIG_IDF_TARGET_ESP32S31
