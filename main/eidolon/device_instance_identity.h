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

// The canonical document this device signs to show it holds the key its issued
// base identity is bound to. The base identity is never this device's own
// invention: it arrives in the commissioning voucher the Host signed, and a
// device that has none has nothing to say here.
std::string BaseIdentityEvidenceDocument(
    const std::string& device_base_id,
    const std::string& device_instance_id,
    const std::string& operational_public_key);

// The canonical document a Body signs to continue one Claim lifecycle without a
// Controller present — after a reconnect, an app-partition reflash, or a
// voucher that expired before the Host could be reached. It is not a way back
// into the queue for a Body that was rejected or removed: that needs a person.
std::string EnrolledBaseKeyDocument(
    const std::string& device_base_id,
    const std::string& device_instance_id,
    const std::string& owner_domain_id,
    const std::string& nonce);

}  // namespace eidolon

#endif
