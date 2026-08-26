#ifndef EIDOLON_DEVICE_INSTANCE_IDENTITY_H_
#define EIDOLON_DEVICE_INSTANCE_IDENTITY_H_

#include <string>

#include "device_foundation_v1_generated.h"

namespace eidolon {

// `hex` is SHA-256(DER SubjectPublicKeyInfo), lowercase and exactly 64 chars.
// Empty when it is not that, which is how the caller learns the key was
// unusable. The rule itself lives in the generated contract header: this used
// to build the string here, agreeing with Hub only because nobody had changed
// either side yet.
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
