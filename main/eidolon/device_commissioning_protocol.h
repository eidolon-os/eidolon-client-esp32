#ifndef EIDOLON_DEVICE_COMMISSIONING_PROTOCOL_H_
#define EIDOLON_DEVICE_COMMISSIONING_PROTOCOL_H_

#include <functional>
#include <string>

namespace eidolon {

// What one commissioning act asked this device to become.
//
// The network is optional and the Host is not: a device already on the right
// Wi-Fi may still need to be told which Host it belongs to, but no commissioning
// payload is meaningful without naming one.
struct CommissioningIntent {
    std::string hub_id;
    std::string certificate_pem;
    bool changes_network = false;
    std::string ssid;
    std::string password;
};

// Read a commissioning payload, accepting only a complete and bounded one.
// Kept apart from the HTTP server so the rules that decide what this device
// will believe can be exercised without a network stack.
bool ParseCommissioningIntent(const std::string& body, CommissioningIntent& out);

// Whether this is something that can serve as a Host's certificate at all.
// Bounded because a hostile payload must not be able to fill the NVS partition,
// and prefix-checked because storing anything else guarantees a later TLS
// failure with no explanation of who supplied the garbage.
bool IsCommissionableCertificate(const std::string& certificate_pem);

// Whether a Hub that answered discovery is the one this device was given.
// An uncommissioned device matches nothing: no Hub is its Hub until a person
// says so.
bool IsCommissionedHub(const std::string& commissioned_hub_id,
                       const std::string& discovered_hub_id);

// The one answer a commissioner gets. It reports that the payload was accepted
// and stored — not that the network came up, which has not been attempted yet
// when this is sent.
struct CommissioningReply {
    std::string status;  // HTTP status line
    std::string body;    // JSON
};

// What one commissioning act does to this device. Supplied by the caller so the
// order these run in — which is the whole contract — can be exercised on a host
// with no network stack and no flash.
struct CommissioningEffects {
    // Remember which Host this device belongs to. False if the certificate was
    // not stored, which stops the act: a device that does not know whose it is
    // has no business joining anyone's network.
    std::function<bool(const std::string& hub_id, const std::string& certificate_pem)> save_trust;
    // Answer the commissioner. Runs before any network change, because the
    // answer travels over the access point that changing networks tears down.
    std::function<void(const CommissioningReply& reply)> reply;
    // Leave for the commissioned network. Called only when the payload carried
    // credentials, and only after the answer has been handed over.
    std::function<void(const std::string& ssid, const std::string& password)> join_network;
};

// Perform one commissioning act: store the Host, answer, then hand over the
// network change. `device_id` goes into the answer so the commissioner can
// recognize the enrollment this device is about to create.
void Commission(const std::string& body, const std::string& device_id,
                const CommissioningEffects& effects);

}  // namespace eidolon

#endif  // EIDOLON_DEVICE_COMMISSIONING_PROTOCOL_H_
