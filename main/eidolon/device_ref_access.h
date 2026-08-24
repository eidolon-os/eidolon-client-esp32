#ifndef EIDOLON_DEVICE_REF_ACCESS_H_
#define EIDOLON_DEVICE_REF_ACCESS_H_

#include <string>

#include "device_foundation_v1_generated.h"

namespace eidolon {

inline const std::string& DeviceRefOwnerDomainId(
    const device_foundation::v1::DeviceRef& ref) {
    return ref.owner_domain_id.value;
}

inline void SetDeviceRefOwnerDomainId(
    device_foundation::v1::DeviceRef& ref, const std::string& value) {
    ref.owner_domain_id.value = value;
}

inline bool SameDeviceRef(const device_foundation::v1::DeviceRef& left,
                          const device_foundation::v1::DeviceRef& right) {
    return left.device_instance_id == right.device_instance_id &&
           DeviceRefOwnerDomainId(left) == DeviceRefOwnerDomainId(right) &&
           left.owner_domain_generation == right.owner_domain_generation &&
           left.claim_generation == right.claim_generation &&
           left.trust_epoch == right.trust_epoch;
}

}  // namespace eidolon

#endif  // EIDOLON_DEVICE_REF_ACCESS_H_
