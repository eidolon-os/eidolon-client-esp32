#include "eidolon/claim_grant_hpke_schedule.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <string>
#include <vector>

namespace {

std::vector<unsigned char> Hex(const std::string& value)
{
    assert(value.size() % 2 == 0);
    const auto nibble = [](char digit) -> unsigned char {
        if (digit >= '0' && digit <= '9') return digit - '0';
        if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
        assert(false);
        return 0;
    };
    std::vector<unsigned char> decoded(value.size() / 2);
    for (size_t index = 0; index < decoded.size(); ++index) {
        decoded[index] = static_cast<unsigned char>(
            nibble(value[index * 2]) * 16 + nibble(value[index * 2 + 1]));
    }
    return decoded;
}

bool HmacSha256(const std::vector<unsigned char>& key,
                const std::vector<unsigned char>& input,
                std::array<unsigned char, 32>& output)
{
    unsigned int size = 0;
    return HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
                input.data(), input.size(), output.data(), &size) != nullptr &&
        size == output.size();
}

std::vector<unsigned char> Bytes(
    const std::array<unsigned char, 32>& value)
{
    return {value.begin(), value.end()};
}

void Rfc9180AppendixA31ScheduleIsExact()
{
    // RFC 9180 A.3.1. The raw ECDH value is derived from skRm and pkEm in
    // that vector; every other expected value is copied from the frozen SDK
    // golden vector RFC9180-A.3.1-BASE.
    const auto raw_dh =
        Hex("13f918529458d2542531406888c8a6d4ea7ff473a6f4db452ac3c4ae1d01cea1");
    const auto enc = Hex(
        "04a92719c6195d5085104f469a8b9814d5838ff72b60501e2c4466e5e67b325ac"
        "98536d7b61a1af4b78e5b7f951c0900be863c403ce65c9bfcb9382657222d18c4");
    const auto recipient = Hex(
        "04fe8c19ce0905191ebc298a9245792531f26f0cece2460639e8bc39cb7f706a82"
        "6a779b4cf969b8a0e539c7f62fb3d30ad6aa8f80e30f1d128aafd68a2ce72ea0");
    const auto info_bytes =
        Hex("4f6465206f6e2061204772656369616e2055726e");
    const std::string info(info_bytes.begin(), info_bytes.end());
    eidolon::P256HpkeBaseContext context;
    assert(eidolon::DeriveP256HpkeBaseContext(
        raw_dh, enc, recipient, info, HmacSha256, context));
    assert(Bytes(context.shared_secret) ==
           Hex("c0d26aeab536609a572b07695d933b589dcf363ff9d93c93adea537aeabb8cb8"));
    assert(Bytes(context.secret) ==
           Hex("2eb7b6bf138f6b5aff857414a058a3f1750054a9ba1f72c2cf0684a6f20b10e1"));
    assert(context.key == Hex("868c066ef58aae6dc589b6cfdd18f97e"));
    assert(context.base_nonce == Hex("4e0bc5018beba4bf004cca59"));
}

void MalformedKemInputsFailClosed()
{
    eidolon::P256HpkeBaseContext context;
    assert(!eidolon::DeriveP256HpkeBaseContext(
        {}, std::vector<unsigned char>(65), std::vector<unsigned char>(65),
        "eidolon-trust-p256-hpke-v1", HmacSha256, context));
    assert(!eidolon::DeriveP256HpkeBaseContext(
        std::vector<unsigned char>(32), std::vector<unsigned char>(64),
        std::vector<unsigned char>(65), "eidolon-trust-p256-hpke-v1",
        HmacSha256, context));
}

}  // namespace

int main()
{
    Rfc9180AppendixA31ScheduleIsExact();
    MalformedKemInputsFailClosed();
    return 0;
}
