#ifndef EIDOLON_AUTHORITY_LOCATOR_H_
#define EIDOLON_AUTHORITY_LOCATOR_H_

#include <esp_err.h>

#include <string>

#include "device_foundation_v1_generated.h"

namespace eidolon {

// Verify an offline-issued descriptor against the commissioned Owner root and
// delegated authority certificate. Transport discovery never enters this
// function as trust material.
esp_err_t VerifyOwnerDomainDescriptor(
    const device_foundation::v1::OwnerDomainDescriptor& descriptor,
    const std::string& canonical_signing_bytes,
    const std::string& owner_root_certificate_pem,
    const std::string& authority_signing_certificate_pem);

}  // namespace eidolon

#endif  // EIDOLON_AUTHORITY_LOCATOR_H_
