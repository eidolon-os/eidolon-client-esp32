#ifndef EIDOLON_DEVICE_ERASE_IDENTITY_ADAPTER_H_
#define EIDOLON_DEVICE_ERASE_IDENTITY_ADAPTER_H_

#include "device_local_erase_core.h"

namespace eidolon {

class DeviceIdentityEraseAckSigner final : public DeviceEraseAckSignerPort {
public:
    bool SignCanonical(const std::string& canonical,
                       std::string& signature) override;
};

}  // namespace eidolon

#endif  // EIDOLON_DEVICE_ERASE_IDENTITY_ADAPTER_H_
