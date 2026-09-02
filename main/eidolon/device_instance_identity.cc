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
    const auto identity =
        device_foundation::v1::DeviceInstanceId::FromSpkiSha256Hex(hex);
    return identity ? identity->value() : std::string{};
}

std::string BaseIdentityEvidenceDocument(
    const std::string& device_base_id,
    const std::string& device_instance_id,
    const std::string& operational_public_key) {
    if (!EvidenceAtom(device_base_id) ||
        !EvidenceAtom(device_instance_id) ||
        !EvidenceAtom(operational_public_key)) {
        return {};
    }
    // Keys are RFC 8785/JCS lexical order and values need no escaping after the
    // restricted identifier check above.
    return "{\"device_base_id\":\"" + device_base_id +
        "\",\"device_instance_id\":\"" + device_instance_id +
        "\",\"operational_public_key\":\"" + operational_public_key +
        "\",\"profile_id\":\"eidolon-trust-p256-hpke-v1\"}";
}

std::string EnrolledBaseKeyDocument(
    const std::string& device_base_id,
    const std::string& device_instance_id,
    const std::string& owner_domain_id,
    const std::string& nonce) {
    if (!EvidenceAtom(device_base_id) ||
        !EvidenceAtom(device_instance_id) ||
        !EvidenceAtom(owner_domain_id) ||
        !EvidenceAtom(nonce)) {
        return {};
    }
    return
        "{\"contract\":\"eidolon.device-foundation.enrolled-base-key-v1\""
        ",\"device_base_id\":\"" + device_base_id +
        "\",\"device_instance_id\":\"" + device_instance_id +
        "\",\"nonce\":\"" + nonce +
        "\",\"owner_domain_id\":\"" + owner_domain_id + "\"}";
}

}  // namespace eidolon
