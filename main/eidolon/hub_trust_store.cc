#include "hub_trust_store.h"

#include "device_provisioning_protocol.h"
#include "hub_types.h"

#include <esp_log.h>
#include <mbedtls/sha256.h>
#include <nvs.h>

#include <array>
#include <cstring>

#define TAG "OwnerTrustStore"

namespace eidolon {

namespace {

constexpr const char* kActiveSlotKey = "trust_active";
constexpr const char* kStagedSlotKey = "trust_stage";
constexpr const char* kStagedGenerationKey = "trust_sgen";

// Slot keys stay below NVS's 15-character key limit. A bundle is first written
// and validated in the inactive slot; only then does one final committed marker
// make it visible. A reset at any earlier instruction preserves the old slot.
constexpr const char* kOwnerKeys[] = {"ot0_owner", "ot1_owner"};
constexpr const char* kRootKeys[] = {"ot0_root", "ot1_root"};
constexpr const char* kAuthorityKeys[] = {"ot0_auth", "ot1_auth"};
constexpr const char* kDescriptorKeys[] = {"ot0_desc", "ot1_desc"};
constexpr const char* kDigestKeys[] = {"ot0_digest", "ot1_digest"};

// Removed development-contract keys. They are erased by Clear but never read:
// V1 is a direct cutover and must not silently revive a partial legacy bundle.
constexpr const char* kLegacyKeys[] = {
    "owner_domain", "owner_root", "authority_cert", "owner_desc"};

class NvsHandle {
public:
    explicit NvsHandle(nvs_open_mode_t mode)
    {
        result_ = nvs_open(kNvsNamespace, mode, &handle_);
    }
    ~NvsHandle()
    {
        if (result_ == ESP_OK) nvs_close(handle_);
    }

    bool valid() const { return result_ == ESP_OK; }
    nvs_handle_t get() const { return handle_; }

private:
    nvs_handle_t handle_ = 0;
    esp_err_t result_ = ESP_FAIL;
};

std::string ReadString(nvs_handle_t handle, const char* key)
{
    size_t length = 0;
    if (nvs_get_str(handle, key, nullptr, &length) != ESP_OK || length == 0) {
        return {};
    }
    std::string value(length, '\0');
    if (nvs_get_str(handle, key, value.data(), &length) != ESP_OK) return {};
    while (!value.empty() && value.back() == '\0') value.pop_back();
    return value;
}

esp_err_t WriteString(nvs_handle_t handle, const char* key,
                      const std::string& value)
{
    return nvs_set_str(handle, key, value.c_str());
}

void AppendDigestField(std::string& material, const std::string& value)
{
    material += std::to_string(value.size());
    material.push_back(':');
    material += value;
    material.push_back('\0');
}

std::string BundleDigest(const OwnerTrustBundle& bundle)
{
    std::string material;
    material.reserve(bundle.owner_domain_id.size() +
                     bundle.owner_domain_descriptor_json.size() +
                     bundle.owner_root_certificate_pem.size() +
                     bundle.authority_signing_certificate_pem.size() + 64);
    AppendDigestField(material, bundle.owner_domain_id);
    AppendDigestField(material, bundle.owner_domain_descriptor_json);
    AppendDigestField(material, bundle.owner_root_certificate_pem);
    AppendDigestField(material, bundle.authority_signing_certificate_pem);

    std::array<unsigned char, 32> digest{};
    if (mbedtls_sha256(
            reinterpret_cast<const unsigned char*>(material.data()),
            material.size(), digest.data(), 0) != 0) {
        return {};
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(64);
    for (const unsigned char byte : digest) {
        encoded.push_back(hex[byte >> 4]);
        encoded.push_back(hex[byte & 0x0f]);
    }
    return encoded;
}

bool ValidBundleShape(const OwnerTrustBundle& bundle)
{
    return !bundle.owner_domain_id.empty() &&
           !bundle.owner_domain_descriptor_json.empty() &&
           IsCommissionableCertificate(bundle.owner_root_certificate_pem) &&
           IsCommissionableCertificate(
               bundle.authority_signing_certificate_pem);
}

bool ReadSlot(nvs_handle_t handle, int slot, OwnerTrustBundle& bundle)
{
    if (slot < 0 || slot > 1) return false;
    bundle = OwnerTrustBundle{};
    bundle.owner_domain_id = ReadString(handle, kOwnerKeys[slot]);
    bundle.owner_root_certificate_pem = ReadString(handle, kRootKeys[slot]);
    bundle.authority_signing_certificate_pem =
        ReadString(handle, kAuthorityKeys[slot]);
    bundle.owner_domain_descriptor_json =
        ReadString(handle, kDescriptorKeys[slot]);
    const std::string stored_digest = ReadString(handle, kDigestKeys[slot]);
    return ValidBundleShape(bundle) && !stored_digest.empty() &&
           stored_digest == BundleDigest(bundle);
}

esp_err_t WriteSlot(nvs_handle_t handle, int slot,
                    const OwnerTrustBundle& bundle)
{
    const std::string digest = BundleDigest(bundle);
    if (digest.empty()) return ESP_FAIL;
    esp_err_t result = WriteString(handle, kOwnerKeys[slot],
                                   bundle.owner_domain_id);
    if (result == ESP_OK) {
        result = WriteString(handle, kRootKeys[slot],
                             bundle.owner_root_certificate_pem);
    }
    if (result == ESP_OK) {
        result = WriteString(handle, kAuthorityKeys[slot],
                             bundle.authority_signing_certificate_pem);
    }
    if (result == ESP_OK) {
        result = WriteString(handle, kDescriptorKeys[slot],
                             bundle.owner_domain_descriptor_json);
    }
    if (result == ESP_OK) {
        result = WriteString(handle, kDigestKeys[slot], digest);
    }
    return result;
}

int ActiveSlot(nvs_handle_t handle)
{
    const std::string active = ReadString(handle, kActiveSlotKey);
    if (active == "0") return 0;
    if (active == "1") return 1;
    return -1;
}

int StagedSlot(nvs_handle_t handle)
{
    const std::string staged = ReadString(handle, kStagedSlotKey);
    if (staged == "0") return 0;
    if (staged == "1") return 1;
    return -1;
}

bool StagedGeneration(nvs_handle_t handle, uint32_t& generation)
{
    generation = 0;
    return nvs_get_u32(handle, kStagedGenerationKey, &generation) == ESP_OK &&
           generation != 0;
}

}  // namespace

OwnerTrustStoreResult OwnerTrustStore::Stage(
    const OwnerTrustBundle& bundle,
    uint32_t setup_generation,
    const std::function<bool()>& commit_guard)
{
    if (!ValidBundleShape(bundle)) return OwnerTrustStoreResult::Invalid;
    if (setup_generation == 0 || !commit_guard || !commit_guard()) {
        return OwnerTrustStoreResult::Stale;
    }

    NvsHandle nvs(NVS_READWRITE);
    if (!nvs.valid()) return OwnerTrustStoreResult::Unavailable;

    const int current_slot = ActiveSlot(nvs.get());
    const int staging_slot = current_slot == 0 ? 1 : 0;
    esp_err_t result = WriteSlot(nvs.get(), staging_slot, bundle);
    if (result == ESP_OK) result = nvs_commit(nvs.get());
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not stage Owner trust: %s",
                 esp_err_to_name(result));
        return OwnerTrustStoreResult::Unavailable;
    }

    OwnerTrustBundle verified;
    if (!ReadSlot(nvs.get(), staging_slot, verified) ||
        verified.owner_domain_id != bundle.owner_domain_id) {
        ESP_LOGE(TAG, "Staged Owner trust did not validate");
        return OwnerTrustStoreResult::Unavailable;
    }

    // This marker exposes only a recoverable staging record. It does not alter
    // the active Owner root, so a reset or failed candidate network cannot
    // leave a half-commissioned device.
    if (!commit_guard()) return OwnerTrustStoreResult::Stale;
    result = nvs_set_str(nvs.get(), kStagedSlotKey,
                         staging_slot == 0 ? "0" : "1");
    if (result == ESP_OK) {
        result = nvs_set_u32(nvs.get(), kStagedGenerationKey,
                             setup_generation);
    }
    if (result == ESP_OK) result = nvs_commit(nvs.get());
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not publish Owner trust staging record: %s",
                 esp_err_to_name(result));
        return OwnerTrustStoreResult::Unavailable;
    }
    ESP_LOGI(TAG, "Staged Owner Domain %s for setup generation %lu",
             bundle.owner_domain_id.c_str(),
             static_cast<unsigned long>(setup_generation));
    return OwnerTrustStoreResult::Staged;
}

bool OwnerTrustStore::LoadStaged(
    uint32_t setup_generation,
    OwnerTrustBundle& bundle) const
{
    bundle = OwnerTrustBundle{};
    NvsHandle nvs(NVS_READONLY);
    if (!nvs.valid()) return false;
    uint32_t stored_generation = 0;
    return StagedGeneration(nvs.get(), stored_generation) &&
           stored_generation == setup_generation &&
           ReadSlot(nvs.get(), StagedSlot(nvs.get()), bundle);
}

OwnerTrustStoreResult OwnerTrustStore::CommitStaged(
    uint32_t setup_generation,
    const std::function<bool()>& commit_guard)
{
    if (setup_generation == 0 || !commit_guard || !commit_guard()) {
        return OwnerTrustStoreResult::Stale;
    }
    NvsHandle nvs(NVS_READWRITE);
    if (!nvs.valid()) return OwnerTrustStoreResult::Unavailable;
    uint32_t stored_generation = 0;
    const int staged_slot = StagedSlot(nvs.get());
    OwnerTrustBundle verified;
    if (!StagedGeneration(nvs.get(), stored_generation) ||
        stored_generation != setup_generation || staged_slot < 0 ||
        !ReadSlot(nvs.get(), staged_slot, verified)) {
        return OwnerTrustStoreResult::Invalid;
    }
    if (!commit_guard()) return OwnerTrustStoreResult::Stale;
    esp_err_t result = nvs_set_str(nvs.get(), kActiveSlotKey,
                                   staged_slot == 0 ? "0" : "1");
    if (result == ESP_OK) result = nvs_erase_key(nvs.get(), kStagedSlotKey);
    if (result == ESP_OK) {
        result = nvs_erase_key(nvs.get(), kStagedGenerationKey);
    }
    if (result == ESP_OK) result = nvs_commit(nvs.get());
    if (result != ESP_OK) return OwnerTrustStoreResult::Unavailable;
    ESP_LOGI(TAG, "Activated Owner Domain %s for setup generation %lu",
             verified.owner_domain_id.c_str(),
             static_cast<unsigned long>(setup_generation));
    return OwnerTrustStoreResult::Staged;
}

OwnerTrustStoreResult OwnerTrustStore::RollbackStaged(
    uint32_t setup_generation)
{
    NvsHandle nvs(NVS_READWRITE);
    if (!nvs.valid()) return OwnerTrustStoreResult::Unavailable;
    uint32_t stored_generation = 0;
    if (!StagedGeneration(nvs.get(), stored_generation)) {
        return OwnerTrustStoreResult::Staged;
    }
    if (stored_generation != setup_generation) {
        return OwnerTrustStoreResult::Stale;
    }
    esp_err_t result = nvs_erase_key(nvs.get(), kStagedSlotKey);
    if (result == ESP_OK) {
        result = nvs_erase_key(nvs.get(), kStagedGenerationKey);
    }
    if (result == ESP_OK) result = nvs_commit(nvs.get());
    return result == ESP_OK ? OwnerTrustStoreResult::Staged
                            : OwnerTrustStoreResult::Unavailable;
}

OwnerTrustStoreResult OwnerTrustStore::ReplaceActive(
    const OwnerTrustBundle& bundle,
    const std::function<bool()>& commit_guard)
{
    if (!ValidBundleShape(bundle)) return OwnerTrustStoreResult::Invalid;
    if (!commit_guard || !commit_guard()) return OwnerTrustStoreResult::Stale;

    NvsHandle nvs(NVS_READWRITE);
    if (!nvs.valid()) return OwnerTrustStoreResult::Unavailable;

    const int current_slot = ActiveSlot(nvs.get());
    const int staging_slot = current_slot == 0 ? 1 : 0;
    esp_err_t result = WriteSlot(nvs.get(), staging_slot, bundle);
    if (result == ESP_OK) result = nvs_commit(nvs.get());
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not stage Owner trust: %s",
                 esp_err_to_name(result));
        return OwnerTrustStoreResult::Unavailable;
    }

    OwnerTrustBundle verified;
    if (!ReadSlot(nvs.get(), staging_slot, verified) ||
        verified.owner_domain_id != bundle.owner_domain_id) {
        ESP_LOGE(TAG, "Staged Owner trust did not validate");
        return OwnerTrustStoreResult::Unavailable;
    }

    // The only visibility point. Never move this guard earlier: verification
    // or NVS staging may outlive its transport generation, but commissioning
    // trust after the response deadline would create an ambiguous Owner state.
    if (!commit_guard()) return OwnerTrustStoreResult::Stale;
    result = nvs_set_str(nvs.get(), kActiveSlotKey,
                         staging_slot == 0 ? "0" : "1");
    if (result == ESP_OK) result = nvs_commit(nvs.get());
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not activate staged Owner trust: %s",
                 esp_err_to_name(result));
        return OwnerTrustStoreResult::Unavailable;
    }

    ESP_LOGI(TAG, "Commissioned Owner Domain %s in trust slot %d",
             bundle.owner_domain_id.c_str(), staging_slot);
    return OwnerTrustStoreResult::Staged;
}

bool OwnerTrustStore::Load(OwnerTrustBundle& bundle) const
{
    bundle = OwnerTrustBundle{};
    NvsHandle nvs(NVS_READONLY);
    if (!nvs.valid()) return false;
    return ReadSlot(nvs.get(), ActiveSlot(nvs.get()), bundle);
}

std::string OwnerTrustStore::CommissionedOwnerDomainId() const
{
    OwnerTrustBundle bundle;
    return Load(bundle) ? bundle.owner_domain_id : std::string{};
}

esp_err_t OwnerTrustStore::SaveAcceptedDescriptor(
    const std::string& descriptor_json)
{
    if (descriptor_json.empty()) return ESP_ERR_INVALID_ARG;
    OwnerTrustBundle bundle;
    if (!Load(bundle)) return ESP_ERR_INVALID_STATE;
    bundle.owner_domain_descriptor_json = descriptor_json;
    const OwnerTrustStoreResult result = ReplaceActive(
        bundle, [] { return true; });
    switch (result) {
    case OwnerTrustStoreResult::Staged:
        return ESP_OK;
    case OwnerTrustStoreResult::Invalid:
        return ESP_ERR_INVALID_ARG;
    case OwnerTrustStoreResult::Stale:
        return ESP_ERR_INVALID_STATE;
    case OwnerTrustStoreResult::Unavailable:
        return ESP_FAIL;
    }
    return ESP_FAIL;
}

void OwnerTrustStore::Clear()
{
    NvsHandle nvs(NVS_READWRITE);
    if (!nvs.valid()) return;
    const auto erase = [&](const char* key) {
        const esp_err_t result = nvs_erase_key(nvs.get(), key);
        if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "Could not erase %s: %s", key,
                     esp_err_to_name(result));
        }
    };
    erase(kActiveSlotKey);
    erase(kStagedSlotKey);
    erase(kStagedGenerationKey);
    for (int slot = 0; slot < 2; ++slot) {
        erase(kOwnerKeys[slot]);
        erase(kRootKeys[slot]);
        erase(kAuthorityKeys[slot]);
        erase(kDescriptorKeys[slot]);
        erase(kDigestKeys[slot]);
    }
    for (const char* key : kLegacyKeys) erase(key);
    nvs_commit(nvs.get());
}

}  // namespace eidolon
