#include "commissioning_credential.h"

#include <cstdlib>
#include <vector>

#include <cJSON.h>

namespace eidolon {

namespace {

constexpr size_t kMaxVoucherBytes = 4096;
constexpr char kPurpose[] = "eidolon-commissioning-voucher-v1";

bool Base64UrlDecode(const std::string& encoded, std::string& out) {
    static constexpr signed char kInvalid = -1;
    auto value = [](char ch) -> signed char {
        if (ch >= 'A' && ch <= 'Z') return static_cast<signed char>(ch - 'A');
        if (ch >= 'a' && ch <= 'z') return static_cast<signed char>(ch - 'a' + 26);
        if (ch >= '0' && ch <= '9') return static_cast<signed char>(ch - '0' + 52);
        if (ch == '-') return 62;
        if (ch == '_') return 63;
        return kInvalid;
    };
    out.clear();
    uint32_t accumulator = 0;
    int bits = 0;
    for (const char ch : encoded) {
        const signed char decoded = value(ch);
        if (decoded == kInvalid) return false;
        accumulator = (accumulator << 6) | static_cast<uint32_t>(decoded);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((accumulator >> bits) & 0xFF));
        }
    }
    // Whatever is left is the padding the encoder dropped; it must be zero, or
    // the string was not base64url of whole bytes.
    return bits < 6 && ((accumulator & ((1u << bits) - 1)) == 0);
}

std::string JsonString(const cJSON* object, const char* key) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring != nullptr
        ? std::string(item->valuestring)
        : std::string();
}

}  // namespace

bool ParseCommissioningVoucher(const std::string& voucher,
                               CommissioningCredential& out) {
    if (voucher.empty() || voucher.size() > kMaxVoucherBytes) return false;
    const size_t first = voucher.find('.');
    if (first == std::string::npos) return false;
    const size_t second = voucher.find('.', first + 1);
    if (second == std::string::npos) return false;
    if (voucher.find('.', second + 1) != std::string::npos) return false;

    std::string claims_json;
    if (!Base64UrlDecode(voucher.substr(first + 1, second - first - 1),
                         claims_json)) {
        return false;
    }
    cJSON* claims = cJSON_Parse(claims_json.c_str());
    if (claims == nullptr) return false;
    const std::string purpose = JsonString(claims, "purpose");
    const std::string device_base_id = JsonString(claims, "device_base_id");
    const std::string owner = JsonString(claims, "owner_domain_id");
    const std::string key = JsonString(claims, "operational_spki_sha256");
    const std::string jti = JsonString(claims, "jti");
    const cJSON* expires = cJSON_GetObjectItemCaseSensitive(claims, "exp");
    const bool expires_ok = cJSON_IsNumber(expires) && expires->valuedouble > 0 &&
        expires->valuedouble <= 9007199254740991.0;
    const int64_t expires_at =
        expires_ok ? static_cast<int64_t>(expires->valuedouble) : 0;
    cJSON_Delete(claims);

    if (purpose != kPurpose || device_base_id.empty() || owner.empty() ||
        key.size() != 71 || key.rfind("sha256:", 0) != 0 || jti.empty() ||
        key.find_first_not_of("0123456789abcdef", 7) != std::string::npos ||
        !expires_ok || expires_at <= 0) {
        return false;
    }
    out = {};
    out.device_base_id = device_base_id;
    out.owner_domain_id = owner;
    out.operational_key_id = key;
    out.voucher = voucher;
    out.voucher_jti = jti;
    out.voucher_expires_at_unix = expires_at;
    return true;
}

CommissioningStanding StandingFor(const CommissioningCredential& credential,
                                  int64_t now_unix) {
    if (credential.device_base_id.empty()) return CommissioningStanding::None;
    if (credential.voucher.empty() || credential.voucher_jti.empty()) {
        return CommissioningStanding::EnrolledBaseKey;
    }
    if (now_unix > 0 && credential.voucher_expires_at_unix > 0 &&
        now_unix >= credential.voucher_expires_at_unix) {
        // The base identity is still ours; only the standing to make a *first*
        // Proposal ran out. Whether continuing is allowed is the Hub's call.
        return CommissioningStanding::Expired;
    }
    return CommissioningStanding::Voucher;
}

}  // namespace eidolon
