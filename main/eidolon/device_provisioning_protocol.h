#ifndef EIDOLON_DEVICE_PROVISIONING_PROTOCOL_H_
#define EIDOLON_DEVICE_PROVISIONING_PROTOCOL_H_

#include <cstddef>
#include <optional>
#include <string>

#include "device_foundation_v1_generated.h"

namespace eidolon {

// The Eidolon OS device provisioning contract, device side.
//
// The controller drives one setup act in a fixed order — read the descriptor,
// hand over the Host, hand over the network, collect the enrollment — and the
// order is the controller's to enforce, because the controller is the party that
// knows it. This device only answers each step.
//
// What carries the act is deliberately absent from this file: BLE, SoftAP, or
// whatever a future device class speaks is a transport underneath, and no rule
// here may depend on which one it was.

// Every request on these endpoints must carry at least one byte, `{}` where
// there is nothing to say. The session's own encryption cannot process a
// zero-length payload — it fails before any handler here is reached, so a device
// cannot be forgiving about it and a controller that sends nothing gets an error
// that names decryption rather than the empty request that caused it.

// What this device tells a controller about itself before it belongs to anyone.
//
// The descriptor's fields are the SDK's `SetupDescriptor`, not a table restated
// here. It used to be a table here and a second one in the controller, and the
// two were kept in step by review: this device encoded an endless setup window
// as `expires_in_seconds: 0`, the controller refused any duration it could not
// act on, and every device out of the box was told its own description broke
// the v1 contract. Neither end was wrong about its own half.
//
// Emitted in canonical (sorted-key) order so the bytes can be compared against
// the SDK golden vector — which is what makes a field added to the contract and
// not here a red test rather than a device nobody can commission.
std::string BuildSetupDescriptorJson(
    const device_foundation::v1::SetupDescriptor& descriptor);

// The one thing a controller hands this device: the Owner Domain it belongs to.
//
// Trust and network travel in the same act because a device that joined a
// network without knowing its Host has nothing it can safely talk to there.
// They stay separate messages so the controller can establish trust first and
// still be answered over a transport the network change is about to remove.
struct TrustHandover {
    std::string owner_domain_id;
    std::string owner_domain_descriptor_json;
    std::string owner_root_certificate_pem;
    std::string authority_signing_certificate_pem;
    // Optional. Present when this commissioning is meant to give the device
    // standing to ask for admission — which is every first setup, and every
    // return after the Owner removed or rejected it. Absent when a Body that
    // is already known is only being pointed at a new network, because nothing
    // about its identity is changing.
    std::string commissioning_voucher;
};

// Adapter callbacks use the same limit before allocating/copying input. Keep a
// single value here so the wire parser cannot accept more than the runtime
// boundary was prepared to own.
inline constexpr size_t kMaxTrustHandoverPayloadBytes = 16 * 1024;

// Read a trust handover, accepting only a complete and bounded one. Kept apart
// from any transport so the rules that decide what this device will believe can
// be exercised without a BLE stack or an HTTP server.
bool ParseTrustHandover(const std::string& body, TrustHandover& out);

// Whether this is something that can serve as a Host's certificate at all.
// Bounded because a hostile payload must not be able to fill the NVS partition,
// and prefix-checked because storing anything else guarantees a later TLS
// failure with no explanation of who supplied the garbage.
bool IsCommissionableCertificate(const std::string& certificate_pem);

// Whether a Hub that answered discovery is the one this device was given.
// A device that belongs to no Host matches nothing: no Hub is its Hub until a
// person says so.
bool IsCommissionedOwnerDomain(const std::string& commissioned_owner_domain_id,
                               const std::string& discovered_owner_domain_id);

// The answer to a trust handover. It reports that the payload was verified and
// durably staged, but deliberately not yet activated. Activation is part of the
// later trust+network transaction commit after Owner route validation.
std::string BuildTrustStagedJson(const std::string& device_id,
                                 const std::string& owner_domain_id);

std::string BuildTrustRefusedJson(const char* reason);

// Custom Protocomm status/ack endpoints use the canonical SDK DTOs. The
// status is the only success evidence Mobile may project as network configured;
// an IDF credential callback is intentionally not part of this contract.
std::string BuildCommissioningStatusJson(
    const device_foundation::v1::CommissioningStatusEvidence& evidence);
bool ParseCommissioningTerminalAck(
    const std::string& body,
    device_foundation::v1::CommissioningTerminalAck& out);

// What the controller collects while this device joins the network and enrolls.
// An empty `lifecycle_state` says the enrollment has not happened yet, which is
// an answer rather than an error: the controller is expected to ask again.
std::string BuildEnrollmentReceiptJson(const std::string& device_id,
                                       const std::string& enrollment_id,
                                       const std::string& lifecycle_state);

}  // namespace eidolon

#endif  // EIDOLON_DEVICE_PROVISIONING_PROTOCOL_H_
