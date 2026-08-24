#ifndef EIDOLON_DEVICE_INSTANCE_IDENTITY_H_
#define EIDOLON_DEVICE_INSTANCE_IDENTITY_H_

#include <string>

namespace eidolon {

// `hex` is SHA-256(DER SubjectPublicKeyInfo), lowercase and exactly 64 chars.
std::string DeviceInstanceIdFromSpkiSha256Hex(const std::string& hex);
std::string DevelopmentCommissioningHmacInput(
    const std::string& hardware_lookup_id,
    const std::string& device_instance_id,
    const std::string& owner_domain_id,
    const std::string& nonce);
std::string DevelopmentHardwareEvidenceDocument(
    const std::string& hardware_lookup_id,
    const std::string& device_instance_id,
    const std::string& operational_public_key);

}  // namespace eidolon

#endif
