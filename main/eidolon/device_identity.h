#ifndef EIDOLON_DEVICE_IDENTITY_H_
#define EIDOLON_DEVICE_IDENTITY_H_

#include <esp_err.h>

#include <string>

#include "hub_onboarding_protocol.h"

#include "device_instance_identity.h"

namespace eidolon {

struct SignedRequestHeaders {
    std::string nonce;
    std::string timestamp;
    std::string public_key;
    std::string signature;
};

class DeviceIdentity {
public:
    static DeviceIdentity& GetInstance();

    esp_err_t EnsureKeypair();
    esp_err_t SignRequest(const std::string& method, const std::string& path_query,
                          const std::string& device_id, const std::string& body,
                          SignedRequestHeaders& out);
    esp_err_t SignGetRequest(const std::string& path_query, const std::string& device_id,
                             SignedRequestHeaders& out);
    esp_err_t SignCanonical(const std::string& canonical, std::string& signature);
    // The same key, spelled the two ways the contracts spell it. Device
    // Control's schema forbids a scheme prefix; Admission's carries one. They
    // are not interchangeable on the wire, and picking the wrong one is a
    // rejection with no other symptom, so each says which contract it is for.
    const std::string& DeviceControlPublicKey() const { return public_key_b64_; }

    std::string AdmissionOperationalPublicKey() const {
        return CanonicalOperationalPublicKeySpki(public_key_b64_);
    }
    const std::string& Fingerprint() const { return fingerprint_; }
    const std::string& DeviceInstanceId() const { return device_instance_id_; }
    // The recovery transaction calls this only after it has durably archived
    // the old removal terminal and erased the old key from NVS.  Forgetting the
    // RAM copy prevents the pre-removal principal from surviving until reboot.
    void ForgetCachedKeyAfterPhysicalRecovery();

private:
    DeviceIdentity() = default;

    esp_err_t LoadOrCreateKey();
    esp_err_t CreateKeypair();
    esp_err_t LoadPublicKeyFromPrivateKey();

    std::string private_key_pem_;
    std::string public_key_b64_;
    std::string fingerprint_;
    std::string device_instance_id_;
};

std::string EidolonSignedGetPathQuery(const std::string& url);

}  // namespace eidolon

#endif  // EIDOLON_DEVICE_IDENTITY_H_
