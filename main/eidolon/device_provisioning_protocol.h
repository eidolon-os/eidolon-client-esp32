#ifndef EIDOLON_DEVICE_PROVISIONING_PROTOCOL_H_
#define EIDOLON_DEVICE_PROVISIONING_PROTOCOL_H_

#include <string>

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
struct ProvisioningDescriptor {
    std::string device_id;
    std::string device_kind;
    std::string display_name;
    std::string identity_fingerprint;
    std::string session_id;
    // A duration, not an instant. A device being set up has not joined a network
    // and has no wall clock, so it can say how long this window lasts but not
    // when it ends; the controller turns this into the absolute expiry its own
    // contract carries.
    int expires_in_seconds = 0;
    // Whether this descriptor's identity is bound to a product credential.
    // Development and production devices differ in this value only — the act
    // that follows is the same one either way.
    bool manufacturer_bound = false;
};

std::string BuildProvisioningDescriptorJson(const ProvisioningDescriptor& descriptor);

// The one thing a controller hands this device: the Host it belongs to.
//
// Trust and network travel in the same act because a device that joined a
// network without knowing its Host has nothing it can safely talk to there.
// They stay separate messages so the controller can establish trust first and
// still be answered over a transport the network change is about to remove.
struct TrustHandover {
    std::string hub_id;
    std::string certificate_pem;
};

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
bool IsCommissionedHub(const std::string& commissioned_hub_id,
                       const std::string& discovered_hub_id);

// The answer to a trust handover. It reports that the payload was accepted and
// stored — not that the network came up or that the Host admitted this device,
// neither of which has been attempted when this is sent.
std::string BuildTrustAcceptedJson(const std::string& device_id, const std::string& hub_id);

std::string BuildTrustRefusedJson(const char* reason);

// What the controller collects while this device joins the network and enrolls.
// An empty `lifecycle_state` says the enrollment has not happened yet, which is
// an answer rather than an error: the controller is expected to ask again.
std::string BuildEnrollmentReceiptJson(const std::string& device_id,
                                       const std::string& enrollment_id,
                                       const std::string& lifecycle_state);

}  // namespace eidolon

#endif  // EIDOLON_DEVICE_PROVISIONING_PROTOCOL_H_
