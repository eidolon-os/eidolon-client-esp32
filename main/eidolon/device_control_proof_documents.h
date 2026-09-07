#ifndef EIDOLON_DEVICE_CONTROL_PROOF_DOCUMENTS_H_
#define EIDOLON_DEVICE_CONTROL_PROOF_DOCUMENTS_H_

#include <string>

#include "device_foundation_v1_generated.h"

namespace eidolon {
namespace device_control {

// The two documents this device signs on the Device Control edge. Neither is
// ever sent: only the signature is, and the Authority rebuilds the bytes from
// what it already holds. So a disagreement about how a member is spelled is
// never reported as a disagreement — it is reported as a signature that did
// not verify, and for the configuration request that means a device holding a
// Claim the Authority calls active which is never given a channel.
//
// They live here, off the ESP-IDF client, so the bytes can be held against
// golden/device-control-configuration-proof.json and
// golden/device-control-manifest-assertion-proof.json without an ESP-IDF
// build. Until that move they were file-local helpers inside
// hub_onboarding_client.cc, which no host suite can compile, so neither had
// ever been checked against anything.
//
// Members are emitted in the order RFC 8785 sorts them, and the DeviceRef is
// the one canonicaliser this firmware has rather than a third copy of it.
std::string ConfigurationProofJson(
    const device_foundation::v1::DeviceRef& device_ref,
    const std::string& nonce);

// The Manifest is bound by its digest, which the Authority recomputes from the
// document it receives, so this signature cannot carry from one set of
// declared capabilities to another.
std::string ManifestAssertionProofJson(
    const device_foundation::v1::DeviceRef& device_ref,
    const std::string& manifest_digest,
    const std::string& nonce);

}  // namespace device_control
}  // namespace eidolon

#endif  // EIDOLON_DEVICE_CONTROL_PROOF_DOCUMENTS_H_
