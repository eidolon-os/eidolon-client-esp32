#include "claim_grant_hpke_schedule.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>

namespace eidolon {
namespace {

std::vector<unsigned char> Bytes(const std::string& value)
{
    return {value.begin(), value.end()};
}

std::vector<unsigned char> Join(
    std::initializer_list<std::vector<unsigned char>> values)
{
    std::vector<unsigned char> output;
    size_t size = 0;
    for (const auto& value : values) size += value.size();
    output.reserve(size);
    for (const auto& value : values) {
        output.insert(output.end(), value.begin(), value.end());
    }
    return output;
}

std::vector<unsigned char> U16(uint16_t value)
{
    return {static_cast<unsigned char>(value >> 8),
            static_cast<unsigned char>(value)};
}

bool HkdfExpand(const std::array<unsigned char, 32>& prk,
                const std::vector<unsigned char>& info, size_t length,
                HmacSha256Function hmac_sha256,
                std::vector<unsigned char>& output)
{
    if (hmac_sha256 == nullptr || length > 255 * 32) return false;
    output.clear();
    std::vector<unsigned char> previous;
    const std::vector<unsigned char> key(prk.begin(), prk.end());
    for (uint16_t counter = 1; output.size() < length; ++counter) {
        std::vector<unsigned char> block_input = previous;
        block_input.insert(block_input.end(), info.begin(), info.end());
        block_input.push_back(static_cast<unsigned char>(counter));
        std::array<unsigned char, 32> block{};
        if (!hmac_sha256(key, block_input, block)) return false;
        previous.assign(block.begin(), block.end());
        const size_t take = std::min(previous.size(), length - output.size());
        output.insert(output.end(), previous.begin(), previous.begin() + take);
    }
    return true;
}

bool LabeledExtract(const std::vector<unsigned char>& suite,
                    const std::vector<unsigned char>& salt,
                    const std::string& label,
                    const std::vector<unsigned char>& ikm,
                    HmacSha256Function hmac_sha256,
                    std::array<unsigned char, 32>& output)
{
    return hmac_sha256 != nullptr &&
        hmac_sha256(salt,
                    Join({Bytes("HPKE-v1"), suite, Bytes(label), ikm}),
                    output);
}

bool LabeledExpand(const std::vector<unsigned char>& suite,
                   const std::array<unsigned char, 32>& prk,
                   const std::string& label,
                   const std::vector<unsigned char>& info, size_t length,
                   HmacSha256Function hmac_sha256,
                   std::vector<unsigned char>& output)
{
    return HkdfExpand(
        prk,
        Join({U16(static_cast<uint16_t>(length)), Bytes("HPKE-v1"), suite,
              Bytes(label), info}),
        length, hmac_sha256, output);
}

}  // namespace

bool DeriveP256HpkeBaseContext(
    const std::vector<unsigned char>& raw_dh,
    const std::vector<unsigned char>& encapsulated_public_key,
    const std::vector<unsigned char>& recipient_public_key,
    const std::string& info,
    HmacSha256Function hmac_sha256,
    P256HpkeBaseContext& context)
{
    context = {};
    if (raw_dh.size() != 32 || encapsulated_public_key.size() != 65 ||
        recipient_public_key.size() != 65 || hmac_sha256 == nullptr) {
        return false;
    }
    const auto kem_suite = Join({Bytes("KEM"), U16(16)});
    std::array<unsigned char, 32> eae_prk{};
    if (!LabeledExtract(kem_suite, {}, "eae_prk", raw_dh, hmac_sha256,
                        eae_prk)) {
        return false;
    }
    std::vector<unsigned char> kem_context = encapsulated_public_key;
    kem_context.insert(kem_context.end(), recipient_public_key.begin(),
                       recipient_public_key.end());
    std::vector<unsigned char> shared;
    if (!LabeledExpand(kem_suite, eae_prk, "shared_secret", kem_context, 32,
                       hmac_sha256, shared)) {
        return false;
    }
    std::copy(shared.begin(), shared.end(), context.shared_secret.begin());

    const auto suite = Join({Bytes("HPKE"), U16(16), U16(1), U16(1)});
    std::array<unsigned char, 32> psk_hash{};
    std::array<unsigned char, 32> info_hash{};
    if (!LabeledExtract(suite, {}, "psk_id_hash", {}, hmac_sha256, psk_hash) ||
        !LabeledExtract(suite, {}, "info_hash", Bytes(info), hmac_sha256,
                        info_hash)) {
        return false;
    }
    std::vector<unsigned char> schedule_context{0};
    schedule_context.insert(schedule_context.end(), psk_hash.begin(),
                            psk_hash.end());
    schedule_context.insert(schedule_context.end(), info_hash.begin(),
                            info_hash.end());
    if (!LabeledExtract(suite, shared, "secret", {}, hmac_sha256,
                        context.secret) ||
        !LabeledExpand(suite, context.secret, "key", schedule_context, 16,
                       hmac_sha256, context.key) ||
        !LabeledExpand(suite, context.secret, "base_nonce", schedule_context,
                       12, hmac_sha256, context.base_nonce)) {
        context = {};
        return false;
    }
    return true;
}

}  // namespace eidolon
