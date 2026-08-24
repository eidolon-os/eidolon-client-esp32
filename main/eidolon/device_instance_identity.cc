#include "device_instance_identity.h"

namespace eidolon {

namespace {
bool EvidenceAtom(const std::string& value) {
    if (value.empty()) return false;
    for (const unsigned char ch : value) {
        const bool allowed = (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == ':' || ch == '.';
        if (!allowed) return false;
    }
    return true;
}
}  // namespace

std::string DeviceInstanceIdFromSpkiSha256Hex(const std::string& hex) {
    if (hex.size() != 64 ||
        hex.find_first_not_of("0123456789abcdef") != std::string::npos) {
        return {};
    }
    return "device-instance-" + hex;
}

std::string DevelopmentCommissioningHmacInput(
    const std::string& hardware_lookup_id,
    const std::string& device_instance_id,
    const std::string& owner_domain_id,
    const std::string& nonce) {
    std::string result = hardware_lookup_id;
    result.push_back('\0');
    result += device_instance_id;
    result.push_back('\0');
    result += owner_domain_id;
    result.push_back('\0');
    result += nonce;
    return result;
}

std::string DevelopmentHardwareEvidenceDocument(
    const std::string& hardware_lookup_id,
    const std::string& device_instance_id,
    const std::string& operational_public_key) {
    if (!EvidenceAtom(hardware_lookup_id) ||
        !EvidenceAtom(device_instance_id) ||
        !EvidenceAtom(operational_public_key)) {
        return {};
    }
    // Keys are RFC 8785/JCS lexical order and values need no escaping after the
    // restricted identifier check above.
    return "{\"device_instance_id\":\"" + device_instance_id +
        "\",\"hardware_lookup_id\":\"" + hardware_lookup_id +
        "\",\"operational_public_key\":\"" + operational_public_key +
        "\",\"profile_id\":\"eidolon-trust-p256-hpke-v1\"}";
}

}  // namespace eidolon
