#ifndef EIDOLON_HUB_TRUST_STORE_H_
#define EIDOLON_HUB_TRUST_STORE_H_

#include <string>

#include <esp_err.h>

namespace eidolon {

struct OwnerTrustBundle {
    std::string owner_domain_id;
    std::string owner_domain_descriptor_json;
    std::string owner_root_certificate_pem;
    std::string authority_signing_certificate_pem;
};

// Commissioning is the only writer of the Owner trust anchor. Discovery may
// later update a signed directory, but can never replace these certificates.
class OwnerTrustStore {
public:
    esp_err_t Save(const OwnerTrustBundle& bundle);
    bool Load(OwnerTrustBundle& bundle) const;
    std::string CommissionedOwnerDomainId() const;
    esp_err_t SaveAcceptedDescriptor(const std::string& descriptor_json);
    void Clear();
};

}  // namespace eidolon

#endif  // EIDOLON_HUB_TRUST_STORE_H_
