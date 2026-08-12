#ifndef EIDOLON_DEVICE_COMMISSIONING_PROTOCOL_H_
#define EIDOLON_DEVICE_COMMISSIONING_PROTOCOL_H_

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

}  // namespace eidolon

#endif  // EIDOLON_DEVICE_COMMISSIONING_PROTOCOL_H_
