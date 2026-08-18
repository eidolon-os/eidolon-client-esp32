#include "hub_trust_store.h"

#include "device_provisioning_protocol.h"
#include "hub_types.h"
#include "settings.h"

#include <esp_log.h>

#define TAG "OwnerTrustStore"

namespace eidolon {

namespace {

constexpr const char* kOwnerDomainKey = "owner_domain";
constexpr const char* kOwnerRootKey = "owner_root";
constexpr const char* kAuthorityCertificateKey = "authority_cert";
constexpr const char* kOwnerDescriptorKey = "owner_desc";

}  // namespace

esp_err_t OwnerTrustStore::Save(const OwnerTrustBundle& bundle)
{
    if (bundle.owner_domain_id.empty() ||
        bundle.owner_domain_descriptor_json.empty() ||
        !IsCommissionableCertificate(bundle.owner_root_certificate_pem) ||
        !IsCommissionableCertificate(bundle.authority_signing_certificate_pem)) {
        return ESP_ERR_INVALID_ARG;
    }
    Settings settings(kNvsNamespace, true);
    esp_err_t err = settings.SetString(kOwnerDomainKey, bundle.owner_domain_id);
    if (err == ESP_OK) {
        err = settings.SetString(kOwnerRootKey,
                                 bundle.owner_root_certificate_pem);
    }
    if (err == ESP_OK) {
        err = settings.SetString(kAuthorityCertificateKey,
                                 bundle.authority_signing_certificate_pem);
    }
    if (err == ESP_OK) {
        err = settings.SetString(kOwnerDescriptorKey,
                                 bundle.owner_domain_descriptor_json);
    }
    if (err == ESP_OK) err = settings.Commit();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Commissioned Owner Domain %s",
                 bundle.owner_domain_id.c_str());
    }
    return err;
}

bool OwnerTrustStore::Load(OwnerTrustBundle& bundle) const
{
    bundle = OwnerTrustBundle{};
    Settings settings(kNvsNamespace, false);
    bundle.owner_domain_id = settings.GetString(kOwnerDomainKey);
    bundle.owner_root_certificate_pem = settings.GetString(kOwnerRootKey);
    bundle.authority_signing_certificate_pem =
        settings.GetString(kAuthorityCertificateKey);
    bundle.owner_domain_descriptor_json = settings.GetString(kOwnerDescriptorKey);
    return !bundle.owner_domain_id.empty() &&
           !bundle.owner_root_certificate_pem.empty() &&
           !bundle.authority_signing_certificate_pem.empty() &&
           !bundle.owner_domain_descriptor_json.empty();
}

std::string OwnerTrustStore::CommissionedOwnerDomainId() const
{
    Settings settings(kNvsNamespace, false);
    return settings.GetString(kOwnerDomainKey);
}

esp_err_t OwnerTrustStore::SaveAcceptedDescriptor(
    const std::string& descriptor_json)
{
    if (descriptor_json.empty()) return ESP_ERR_INVALID_ARG;
    Settings settings(kNvsNamespace, true);
    esp_err_t err = settings.SetString(kOwnerDescriptorKey, descriptor_json);
    return err == ESP_OK ? settings.Commit() : err;
}

void OwnerTrustStore::Clear()
{
    Settings settings(kNvsNamespace, true);
    settings.EraseKey(kOwnerDomainKey);
    settings.EraseKey(kOwnerRootKey);
    settings.EraseKey(kAuthorityCertificateKey);
    settings.EraseKey(kOwnerDescriptorKey);
    settings.Commit();
}

}  // namespace eidolon
