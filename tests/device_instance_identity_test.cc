#include "eidolon/device_instance_identity.h"

#include <cassert>

int main() {
    const std::string digest =
        "5cd252fb0ce8932436faf8ccd1040981b89ee4ad6b9fe9e2a2b7e71aacb27cd3";
    const std::string first =
        eidolon::DeviceInstanceIdFromSpkiSha256Hex(digest);
    const std::string rebooted =
        eidolon::DeviceInstanceIdFromSpkiSha256Hex(digest);
    assert(first == "device-instance-" + digest);
    assert(rebooted == first);
    assert(eidolon::DeviceInstanceIdFromSpkiSha256Hex("ABC").empty());
    const std::string input = eidolon::DevelopmentCommissioningHmacInput(
        "box-3-golden", first, "owner-domain_01",
        "commissioning-nonce-golden");
    std::string expected = "box-3-golden";
    expected.push_back('\0');
    expected += first;
    expected.push_back('\0');
    expected += "owner-domain_01";
    expected.push_back('\0');
    expected += "commissioning-nonce-golden";
    assert(input == expected);
    const std::string public_key =
        "p256-spki:MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEaxfR8uEsQkf4vOblY6RA8ncDfYEt6zOg9KE5RdiYwpZP40Li_hp_m47n60p8D54WK84zV2sxXs7LtkBoN79R9Q";
    assert(eidolon::DevelopmentHardwareEvidenceDocument(
        "box-3-golden", first, public_key) ==
        "{\"device_instance_id\":\"device-instance-5cd252fb0ce8932436faf8ccd1040981b89ee4ad6b9fe9e2a2b7e71aacb27cd3\",\"hardware_lookup_id\":\"box-3-golden\",\"operational_public_key\":\"p256-spki:MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEaxfR8uEsQkf4vOblY6RA8ncDfYEt6zOg9KE5RdiYwpZP40Li_hp_m47n60p8D54WK84zV2sxXs7LtkBoN79R9Q\",\"profile_id\":\"eidolon-trust-p256-hpke-v1\"}");
}
