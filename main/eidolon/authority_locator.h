#ifndef EIDOLON_AUTHORITY_LOCATOR_H_
#define EIDOLON_AUTHORITY_LOCATOR_H_

#include <esp_err.h>

#include <memory>
#include <string>

#include "device_foundation_v1_generated.h"
#include "hub_trust_store.h"

namespace eidolon {

// Verify an offline-issued descriptor against the commissioned Owner root and
// delegated authority certificate. Transport discovery never enters this
// function as trust material.
esp_err_t VerifyOwnerDomainDescriptor(
    const device_foundation::v1::OwnerDomainDescriptor& descriptor,
    const std::string& canonical_signing_bytes,
    const std::string& owner_root_certificate_pem,
    const std::string& authority_signing_certificate_pem);

// Process-wide device adapter around the pure AuthorityLocatorCore. All
// firmware consumers resolve logical Authorities here; none select a Host,
// address, DNS name, or descriptor endpoint independently.
class DeviceAuthorityLocator {
public:
    static DeviceAuthorityLocator& GetInstance();

    esp_err_t ReloadCommissionedDirectory();
    esp_err_t AcceptDescriptor(
        const device_foundation::v1::OwnerDomainDescriptor& descriptor,
        const std::string& canonical_signing_bytes,
        const std::string& descriptor_json);
    esp_err_t Resolve(
        device_foundation::v1::LogicalAuthority authority,
        device_foundation::v1::AuthorityEndpoint& out) const;
    esp_err_t AcceptedDescriptor(
        device_foundation::v1::OwnerDomainDescriptor& out) const;
    esp_err_t TrustBundle(OwnerTrustBundle& out) const;

    ~DeviceAuthorityLocator();
    DeviceAuthorityLocator(const DeviceAuthorityLocator&) = delete;
    DeviceAuthorityLocator& operator=(const DeviceAuthorityLocator&) = delete;

private:
    DeviceAuthorityLocator();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace eidolon

#endif  // EIDOLON_AUTHORITY_LOCATOR_H_
