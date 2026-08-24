#ifndef EIDOLON_DEVICE_REF_ACCESS_H_
#define EIDOLON_DEVICE_REF_ACCESS_H_

#include <string>

namespace eidolon {

// Transitional accessors keep Core/adapter code independent from the generated
// OwnerDomainId wrapper landing in PH2-B. They do not define or copy a wire
// binding; the generated DeviceRef remains the sole canonical type.
template <typename Ref>
const std::string& DeviceRefOwnerDomainId(const Ref& ref) {
    if constexpr (requires { ref.owner_domain_id.value; }) {
        return ref.owner_domain_id.value;
    } else {
        return ref.owner_domain_id;
    }
}

template <typename Ref>
void SetDeviceRefOwnerDomainId(Ref& ref, const std::string& value) {
    if constexpr (requires { ref.owner_domain_id.value; }) {
        ref.owner_domain_id.value = value;
    } else {
        ref.owner_domain_id = value;
    }
}

template <typename Ref>
std::string DeviceRefLegacyManifestDigest(const Ref& ref) {
    if constexpr (requires { ref.accepted_manifest_digest; }) {
        return ref.accepted_manifest_digest;
    } else {
        return {};
    }
}

template <typename Ref>
void SetDeviceRefLegacyManifestDigest(Ref& ref, const std::string& value) {
    if constexpr (requires { ref.accepted_manifest_digest; }) {
        ref.accepted_manifest_digest = value;
    } else {
        (void)ref;
        (void)value;
    }
}

template <typename Ref>
bool SameDeviceRef(const Ref& left, const Ref& right) {
    return left.device_instance_id == right.device_instance_id &&
           DeviceRefOwnerDomainId(left) == DeviceRefOwnerDomainId(right) &&
           left.owner_domain_generation == right.owner_domain_generation &&
           left.claim_generation == right.claim_generation &&
           left.trust_epoch == right.trust_epoch &&
           DeviceRefLegacyManifestDigest(left) ==
               DeviceRefLegacyManifestDigest(right);
}

}  // namespace eidolon

#endif  // EIDOLON_DEVICE_REF_ACCESS_H_
