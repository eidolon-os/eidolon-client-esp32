#include "esp_idf_commissioning_credential_store.h"

#include <cinttypes>
#include <cstdio>

#include <esp_log.h>

#include "settings.h"

namespace eidolon {

namespace {
constexpr const char* TAG = "EidolonCredential";
// Colocation is the invariant, not a convenience: see the header, which also
// publishes these names so the two operations that end this identity erase all
// of it.
constexpr const char* kNamespace = kCommissioningCredentialNamespace;
constexpr const char* kBaseIdKey = kCommissioningCredentialKeys[0];
constexpr const char* kVoucherKey = kCommissioningCredentialKeys[1];
constexpr const char* kVoucherJtiKey = kCommissioningCredentialKeys[2];
constexpr const char* kVoucherExpKey = kCommissioningCredentialKeys[3];
}  // namespace

EspIdfCommissioningCredentialStore&
EspIdfCommissioningCredentialStore::GetInstance()
{
    static EspIdfCommissioningCredentialStore store;
    return store;
}

bool EspIdfCommissioningCredentialStore::Save(
    const CommissioningCredential& credential)
{
    if (credential.device_base_id.empty()) return false;
    Settings settings(kNamespace, true);
    if (settings.SetString(kBaseIdKey, credential.device_base_id) != ESP_OK ||
        settings.SetString(kVoucherKey, credential.voucher) != ESP_OK ||
        settings.SetString(kVoucherJtiKey, credential.voucher_jti) != ESP_OK) {
        ESP_LOGE(TAG, "Commissioning credential could not be stored");
        return false;
    }
    settings.SetInt(kVoucherExpKey,
                    static_cast<int32_t>(credential.voucher_expires_at_unix));
    return settings.Commit() == ESP_OK;
}

bool EspIdfCommissioningCredentialStore::Load(
    CommissioningCredential& out) const
{
    Settings settings(kNamespace);
    out.device_base_id = settings.GetString(kBaseIdKey);
    out.voucher = settings.GetString(kVoucherKey);
    out.voucher_jti = settings.GetString(kVoucherJtiKey);
    out.voucher_expires_at_unix = settings.GetInt(kVoucherExpKey);
    return !out.device_base_id.empty();
}

bool EspIdfCommissioningCredentialStore::ForgetSpentVoucher()
{
    Settings settings(kNamespace, true);
    if (settings.GetString(kVoucherKey).empty()) return true;
    settings.EraseKey(kVoucherKey);
    settings.EraseKey(kVoucherJtiKey);
    settings.EraseKey(kVoucherExpKey);
    return settings.Commit() == ESP_OK;
}

}  // namespace eidolon
