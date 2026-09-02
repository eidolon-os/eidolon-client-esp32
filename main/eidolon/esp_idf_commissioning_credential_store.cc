#include "esp_idf_commissioning_credential_store.h"

#include <cinttypes>
#include <cstdio>

#include <esp_log.h>

#include "device_identity.h"
#include "settings.h"

namespace eidolon {

namespace {
constexpr const char* TAG = "EidolonCredential";
// The operational key's own namespace. Colocation is the invariant, not a
// convenience: see the header.
constexpr const char* kNamespace = "eidolon_id";
constexpr const char* kBaseIdKey = "base_id";
constexpr const char* kVoucherKey = "voucher";
constexpr const char* kVoucherJtiKey = "voucher_jti";
constexpr const char* kVoucherExpKey = "voucher_exp";
constexpr const char* kKeyFingerprintKey = "op_key_fp";
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
    const std::string fingerprint =
        credential.operational_key_fingerprint.empty()
            ? DeviceIdentity::GetInstance().Fingerprint()
            : credential.operational_key_fingerprint;
    if (fingerprint.empty()) return false;
    Settings settings(kNamespace, true);
    if (settings.SetString(kKeyFingerprintKey, fingerprint) != ESP_OK ||
        settings.SetString(kBaseIdKey, credential.device_base_id) != ESP_OK ||
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
    out.operational_key_fingerprint = settings.GetString(kKeyFingerprintKey);
    out.voucher = settings.GetString(kVoucherKey);
    out.voucher_jti = settings.GetString(kVoucherJtiKey);
    out.voucher_expires_at_unix = settings.GetInt(kVoucherExpKey);
    if (out.device_base_id.empty()) return false;
    const std::string& held = DeviceIdentity::GetInstance().Fingerprint();
    if (held.empty() || out.operational_key_fingerprint != held) {
        ESP_LOGW(TAG,
                 "Stored base identity does not belong to the operational key "
                 "in hand; this device has no commissioning credential");
        out = CommissioningCredential{};
        return false;
    }
    return true;
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
