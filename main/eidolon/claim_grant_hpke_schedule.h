#ifndef EIDOLON_CLAIM_GRANT_HPKE_SCHEDULE_H_
#define EIDOLON_CLAIM_GRANT_HPKE_SCHEDULE_H_

#include <array>
#include <string>
#include <vector>

namespace eidolon {

using HmacSha256Function = bool (*)(
    const std::vector<unsigned char>& key,
    const std::vector<unsigned char>& input,
    std::array<unsigned char, 32>& output);

struct P256HpkeBaseContext {
    std::array<unsigned char, 32> shared_secret{};
    std::array<unsigned char, 32> secret{};
    std::vector<unsigned char> key;
    std::vector<unsigned char> base_nonce;
};

// RFC 9180 Base mode for DHKEM(P-256, HKDF-SHA256), HKDF-SHA256 and
// AES-128-GCM. The caller supplies raw ECDH output and the two uncompressed
// SEC1 public points, keeping curve and hardware-key details outside this pure
// schedule. This function is shared by the ESP adapter and host golden tests.
bool DeriveP256HpkeBaseContext(
    const std::vector<unsigned char>& raw_dh,
    const std::vector<unsigned char>& encapsulated_public_key,
    const std::vector<unsigned char>& recipient_public_key,
    const std::string& info,
    HmacSha256Function hmac_sha256,
    P256HpkeBaseContext& context);

}  // namespace eidolon

#endif  // EIDOLON_CLAIM_GRANT_HPKE_SCHEDULE_H_
