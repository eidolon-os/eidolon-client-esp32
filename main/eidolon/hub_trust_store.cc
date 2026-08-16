#include "hub_trust_store.h"

#include "device_provisioning_protocol.h"
#include "hub_types.h"
#include "settings.h"

#include <esp_log.h>

#define TAG "HubTrustStore"

namespace eidolon {

namespace {

// One Host at a time. A device belongs to the Host it was commissioned for, and
// switching Hosts is a commissioning decision, not something the device may
// accumulate. Storing the Hub id alongside the certificate keeps a stale
// certificate from being offered to a different Hub that happens to answer.
constexpr const char* kCertificateKey = "hub_ca";
constexpr const char* kCertificateHubKey = "hub_ca_id";

}  // namespace

esp_err_t HubTrustStore::Save(const std::string& hub_id,
                              const std::string& certificate_pem)
{
    if (hub_id.empty() || !IsCommissionableCertificate(certificate_pem)) {
        return ESP_ERR_INVALID_ARG;
    }
    Settings settings(kNvsNamespace, true);
    esp_err_t err = settings.SetString(kCertificateKey, certificate_pem);
    if (err != ESP_OK) {
        return err;
    }
    err = settings.SetString(kCertificateHubKey, hub_id);
    if (err != ESP_OK) {
        return err;
    }
    err = settings.Commit();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Trusting Hub %s by commissioned certificate (%u bytes)",
                 hub_id.c_str(), static_cast<unsigned>(certificate_pem.size()));
    }
    return err;
}

std::string HubTrustStore::Load(const std::string& hub_id) const
{
    Settings settings(kNvsNamespace, false);
    const std::string stored_hub = settings.GetString(kCertificateHubKey);
    if (hub_id.empty() || stored_hub != hub_id) {
        return "";
    }
    return settings.GetString(kCertificateKey);
}

std::string HubTrustStore::CommissionedHubId() const
{
    Settings settings(kNvsNamespace, false);
    return settings.GetString(kCertificateHubKey);
}

void HubTrustStore::Clear()
{
    Settings settings(kNvsNamespace, true);
    settings.EraseKey(kCertificateKey);
    settings.EraseKey(kCertificateHubKey);
    settings.Commit();
}

}  // namespace eidolon
