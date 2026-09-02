#include "eidolon/device_instance_identity.h"

#include <cassert>

int main() {
    const std::string digest =
        "410376c9d5dc88022d04b4f31b1e035453ba0c2226887e019fb33a23dca2cbc7";
    const std::string first =
        eidolon::DeviceInstanceIdFromSpkiSha256Hex(digest);
    const std::string rebooted =
        eidolon::DeviceInstanceIdFromSpkiSha256Hex(digest);
    assert(first == "device-instance-" + digest);
    assert(rebooted == first);
    assert(eidolon::DeviceInstanceIdFromSpkiSha256Hex("ABC").empty());

    // The base identity is the Hub's to mint, so the device never builds one;
    // what it builds is the document proving it holds the key that identity was
    // bound to. Both strings are the contract vector's, not this test's idea of
    // them.
    const std::string base_id = "device-base-4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b";
    const std::string public_key =
        "p256-spki:MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEiJhOVtn9mNBt6RSX2zTRLaqeOqzYF-QBB_-ta4MbD0Uacc_T9j4rNfZGNPhseW0U5L5FGsdNfSvzCq-66O9BXw";
    assert(eidolon::BaseIdentityEvidenceDocument(base_id, first, public_key) ==
        "{\"device_base_id\":\"device-base-4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b\",\"device_instance_id\":\"device-instance-410376c9d5dc88022d04b4f31b1e035453ba0c2226887e019fb33a23dca2cbc7\",\"operational_public_key\":\"p256-spki:MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEiJhOVtn9mNBt6RSX2zTRLaqeOqzYF-QBB_-ta4MbD0Uacc_T9j4rNfZGNPhseW0U5L5FGsdNfSvzCq-66O9BXw\",\"profile_id\":\"eidolon-trust-p256-hpke-v1\"}");

    // Continuing one Claim lifecycle without a Controller present.
    assert(eidolon::EnrolledBaseKeyDocument(
        base_id, first, "owner-domain_01", "commissioning-nonce-golden-enrolled") ==
        "{\"contract\":\"eidolon.device-foundation.enrolled-base-key-v1\",\"device_base_id\":\"device-base-4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b\",\"device_instance_id\":\"device-instance-410376c9d5dc88022d04b4f31b1e035453ba0c2226887e019fb33a23dca2cbc7\",\"nonce\":\"commissioning-nonce-golden-enrolled\",\"owner_domain_id\":\"owner-domain_01\"}");

    // A value that could not be a base identity, an instance id or a key is not
    // quietly encoded: the document is empty, and the caller refuses to enroll.
    assert(eidolon::BaseIdentityEvidenceDocument(
        "device-base-\" or 1=1", first, public_key).empty());
    assert(eidolon::EnrolledBaseKeyDocument(base_id, first, "", "n").empty());
}
