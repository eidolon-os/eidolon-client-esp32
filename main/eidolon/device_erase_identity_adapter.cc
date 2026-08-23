#include "device_erase_identity_adapter.h"

#include "device_identity.h"

namespace eidolon {

bool DeviceIdentityEraseAckSigner::SignCanonical(
    const std::string& canonical, std::string& signature) {
    auto& identity = DeviceIdentity::GetInstance();
    return identity.EnsureKeypair() == ESP_OK &&
           identity.SignCanonical(canonical, signature) == ESP_OK;
}

}  // namespace eidolon
