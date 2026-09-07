#include "device_control_proof_documents.h"

#include "device_claim_consumer_core.h"

#include <cstdio>

namespace eidolon {
namespace device_control {
namespace {

// The same escaping the claim consumer applies, kept here rather than reached
// for across a translation unit: a member value that needed escaping and did
// not get it would produce bytes the Authority cannot rebuild.
std::string Quote(const std::string& value) {
    std::string out = "\"";
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20) {
                char escaped[7];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
                out += escaped;
            } else {
                out += static_cast<char>(ch);
            }
        }
    }
    return out + "\"";
}

}  // namespace

std::string ConfigurationProofJson(
    const device_foundation::v1::DeviceRef& device_ref,
    const std::string& nonce) {
    return std::string("{\"device_ref\":") +
           DeviceClaimConsumerCore::DeviceRefJson(device_ref) +
           ",\"nonce\":" + Quote(nonce) +
           ",\"operation_type\":\"device-control.configuration\"}";
}

std::string ManifestAssertionProofJson(
    const device_foundation::v1::DeviceRef& device_ref,
    const std::string& manifest_digest,
    const std::string& nonce) {
    return std::string("{\"device_ref\":") +
           DeviceClaimConsumerCore::DeviceRefJson(device_ref) +
           ",\"manifest_digest\":" + Quote(manifest_digest) +
           ",\"nonce\":" + Quote(nonce) +
           ",\"operation_type\":\"device-control.manifest-assert\"}";
}

}  // namespace device_control
}  // namespace eidolon
