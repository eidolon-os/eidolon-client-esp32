#include "commissioning_transaction.h"
#include "mbedtls_sha256_compat.h"
#include "hub_trust_store.h"
#include "hub_types.h"
#include "hub_onboarding_protocol.h"
#include "esp_idf_commissioning_credential_store.h"
#include "esp_idf_owner_data_erase_storage.h"

#include <ssid_manager.h>
#include <cJSON.h>
#include <nvs.h>
#include <array>
#include <mutex>

namespace eidolon {
namespace {
constexpr const char* kJournal = "ctx_record";
constexpr const char* kNetwork = "ctx_candidate";
enum Phase : uint8_t { CommitDecided = 1, NetworkCommitted, OwnerCleaned, IdentityCommitted, TrustCommitted };
enum class Load { Missing, Loaded, Legacy, Unavailable };
std::mutex transaction_mutex;
struct Journal {
    uint32_t generation = 0;
    uint8_t phase = CommitDecided;
    uint32_t cleanup_index = 0;
    std::string owner, ssid, network_digest, trust_digest, identity_digest;
    bool replacement = false;
};

Load Read(const char* ns, const char* key, std::string& out) {
    out.clear();
    nvs_handle_t handle = 0;
    auto err = nvs_open(ns, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return Load::Missing;
    if (err != ESP_OK) return Load::Unavailable;
    size_t size = 0;
    err = nvs_get_str(handle, key, nullptr, &size);
    if (err == ESP_OK && size > 0 && size < 16384) {
        out.resize(size);
        err = nvs_get_str(handle, key, out.data(), &size);
        if (err == ESP_OK) out.resize(size - 1);
    } else if (err == ESP_OK) err = ESP_FAIL;
    nvs_close(handle);
    return err == ESP_OK ? Load::Loaded : err == ESP_ERR_NVS_NOT_FOUND ? Load::Missing : Load::Unavailable;
}

bool Write(const char* ns, const char* key, const std::string& value) {
    nvs_handle_t handle = 0;
    if (nvs_open(ns, NVS_READWRITE, &handle) != ESP_OK) return false;
    auto err = nvs_set_str(handle, key, value.c_str());
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

bool Erase(const char* ns, const char* key) {
    nvs_handle_t handle = 0;
    auto err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;
    if (err != ESP_OK) return false;
    err = nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

std::string Hash(const std::string& material) {
    std::array<unsigned char, 32> digest{};
    if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(material.data()), material.size(), digest.data(), 0) != 0) return {};
    static constexpr char hex[] = "0123456789abcdef";
    std::string encoded;
    for (const auto byte : digest) { encoded += hex[byte >> 4]; encoded += hex[byte & 15]; }
    return encoded;
}

std::string BundleHash(const OwnerTrustBundle& bundle) {
    std::string material;
    for (const auto* item : {&bundle.owner_domain_id, &bundle.owner_root_certificate_pem,
                            &bundle.authority_signing_certificate_pem, &bundle.owner_domain_descriptor_json}) {
        material += std::to_string(item->size()) + ":" + *item;
    }
    return Hash(material);
}

std::string Text(const cJSON* root, const char* key) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

std::string Print(cJSON* root) {
    char* raw = cJSON_PrintUnformatted(root);
    std::string out = raw ? raw : "";
    cJSON_free(raw);
    cJSON_Delete(root);
    return out;
}

bool Store(const Journal& j) {
    cJSON* root = cJSON_CreateObject();
    if (!root) return false;
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddNumberToObject(root, "generation", j.generation);
    cJSON_AddNumberToObject(root, "phase", j.phase);
    cJSON_AddNumberToObject(root, "cleanup_index", j.cleanup_index);
    cJSON_AddBoolToObject(root, "replacement", j.replacement);
    cJSON_AddStringToObject(root, "owner", j.owner.c_str());
    cJSON_AddStringToObject(root, "ssid", j.ssid.c_str());
    cJSON_AddStringToObject(root, "network_digest", j.network_digest.c_str());
    cJSON_AddStringToObject(root, "trust_digest", j.trust_digest.c_str());
    cJSON_AddStringToObject(root, "identity_digest", j.identity_digest.c_str());
    const auto encoded = Print(root);
    return !encoded.empty() && Write(kNvsNamespace, kJournal, encoded);
}

Load LoadJournal(Journal& j) {
    std::string raw;
    const auto loaded = Read(kNvsNamespace, kJournal, raw);
    if (loaded != Load::Loaded) {
        if (loaded != Load::Missing) return loaded;
        // Legacy journals do not bind a credential/key snapshot. Never infer
        // that an interrupted legacy switch can safely use current credentials.
        nvs_handle_t handle = 0;
        const auto opened = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
        if (opened == ESP_ERR_NVS_NOT_FOUND) return Load::Missing;
        if (opened != ESP_OK) return Load::Unavailable;
        uint32_t generation = 0;
        const auto legacy = nvs_get_u32(handle, "ctx_gen", &generation);
        nvs_close(handle);
        return legacy == ESP_ERR_NVS_NOT_FOUND ? Load::Missing :
            legacy == ESP_OK && generation != 0 ? Load::Legacy : Load::Unavailable;
    }
    cJSON* root = cJSON_ParseWithLength(raw.data(), raw.size());
    uint32_t phase = 0, version = 0;
    const auto number = [&](const char* key, uint32_t& out) {
        const auto* item = cJSON_GetObjectItemCaseSensitive(root, key);
        if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > UINT32_MAX) return false;
        out = static_cast<uint32_t>(item->valuedouble);
        return out == item->valuedouble;
    };
    const auto* replacement = cJSON_GetObjectItemCaseSensitive(root, "replacement");
    const bool valid_numbers = number("version", version) && version == 1 &&
        number("generation", j.generation) && j.generation != 0 &&
        number("phase", phase) && phase >= CommitDecided && phase <= TrustCommitted &&
        number("cleanup_index", j.cleanup_index) && cJSON_IsBool(replacement);
    j.phase = static_cast<uint8_t>(phase);
    j.replacement = cJSON_IsTrue(replacement);
    j.owner = Text(root, "owner"); j.ssid = Text(root, "ssid");
    j.network_digest = Text(root, "network_digest");
    j.trust_digest = Text(root, "trust_digest");
    j.identity_digest = Text(root, "identity_digest");
    cJSON_Delete(root);
    return valid_numbers && !j.owner.empty() && !j.ssid.empty() &&
        j.network_digest.size() == 64 && j.trust_digest.size() == 64 && j.identity_digest.size() == 64
        ? Load::Loaded : Load::Unavailable;
}

bool DurableNetworkMatches(const Journal& journal) {
    for (int i = 0; i < 10; ++i) {
        const auto suffix = i == 0 ? std::string{} : std::to_string(i);
        std::string ssid, password;
        if (Read("wifi", ("ssid" + suffix).c_str(), ssid) == Load::Loaded && ssid == journal.ssid &&
            Read("wifi", ("password" + suffix).c_str(), password) == Load::Loaded &&
            Hash(std::to_string(ssid.size()) + ":" + ssid + password) == journal.network_digest) return true;
    }
    return false;
}

bool Advance(Journal& j, Phase phase) { j.phase = phase; return Store(j); }

bool Finish(Journal& j) {
    auto& credentials = EspIdfCommissioningCredentialStore::GetInstance();
    if (j.phase == CommitDecided) {
        std::string network;
        if (Read("wifi", kNetwork, network) != Load::Loaded) return false;
        cJSON* root = cJSON_ParseWithLength(network.data(), network.size());
        const auto ssid = Text(root, "ssid"), password = Text(root, "password");
        cJSON_Delete(root);
        if (ssid != j.ssid || Hash(std::to_string(ssid.size()) + ":" + ssid + password) != j.network_digest) return false;
        SsidManager::GetInstance().AddSsid(ssid, password);
        if (!DurableNetworkMatches(j) || !Advance(j, NetworkCommitted)) return false;
    }
    if (j.phase == NetworkCommitted) {
        if (j.replacement) {
            const auto plan = EspIdfDeviceLocalEraseAdapter::BuildCommissioningCleanupPlan();
            if (!plan.valid || j.cleanup_index > plan.targets.size()) return false;
            EspIdfOwnerDataEraseStorage storage;
            const auto success = [](auto result) {
                return result == OwnerDataEraseStorageResult::Done || result == OwnerDataEraseStorageResult::NotFound;
            };
            while (j.cleanup_index < plan.targets.size()) {
                const auto& target = plan.targets[j.cleanup_index];
                if (!success(storage.EraseTarget(target)) || !success(storage.VerifyTargetErased(target))) return false;
                ++j.cleanup_index;
                if (!Store(j)) return false;
            }
            // Keep only the network explicitly supplied for this new lifecycle.
            std::string network;
            if (Read("wifi", kNetwork, network) != Load::Loaded) return false;
            cJSON* candidate = cJSON_ParseWithLength(network.data(), network.size());
            const auto ssid = Text(candidate, "ssid"), password = Text(candidate, "password");
            cJSON_Delete(candidate);
            if (ssid != j.ssid || Hash(std::to_string(ssid.size()) + ":" + ssid + password) != j.network_digest) return false;
            auto& networks = SsidManager::GetInstance();
            // A torn legacy profile rewrite can leave duplicate or mismatched
            // SSID/password slots. Rebuild this scoped list from the durable
            // candidate; ctx_candidate survives Clear(), including a reboot.
            networks.Clear();
            networks.AddSsid(ssid, password);
            if (!DurableNetworkMatches(j)) return false;
            for (int i = 1; i < 10; ++i) {
                std::string old;
                if (Read("wifi", ("ssid" + std::to_string(i)).c_str(), old) != Load::Missing) return false;
            }
        }
        if (!Advance(j, OwnerCleaned)) return false;
    }
    if (j.phase == OwnerCleaned) {
        if (!credentials.CommitStaged(j.generation, j.identity_digest) || !Advance(j, IdentityCommitted)) return false;
    }
    if (j.phase == IdentityCommitted) {
        OwnerTrustBundle active;
        if (!OwnerTrustStore().Load(active) || BundleHash(active) != j.trust_digest) {
            OwnerTrustBundle staged;
            if (!OwnerTrustStore().LoadStaged(j.generation, staged) || BundleHash(staged) != j.trust_digest ||
                OwnerTrustStore().CommitStaged(j.generation, [] { return true; }) != OwnerTrustStoreResult::Staged) return false;
        }
        if (!OwnerTrustStore().Load(active) || BundleHash(active) != j.trust_digest || !Advance(j, TrustCommitted)) return false;
    }
    if (j.phase != TrustCommitted) return false;
    OwnerTrustBundle active;
    if (!OwnerTrustStore().Load(active) || BundleHash(active) != j.trust_digest ||
        !credentials.CommitStaged(j.generation, j.identity_digest) ||
        !OwnerTrustStore().DiscardInactive() || !credentials.Finish(j.generation)) return false;
    // Network candidate contains the password and lives in the Wi-Fi store,
    // never in the recovery journal. It is not needed once the network is durable.
    for (const auto* key : {"ctx_gen", "ctx_phase", "ctx_owner", "ctx_ssid", "ctx_digest"}) {
        if (!Erase(kNvsNamespace, key)) return false;
    }
    return Erase("wifi", kNetwork) && Erase(kNvsNamespace, kJournal);
}
}

CommissioningTransactionResult CommitCommissioningTransaction(
    uint32_t generation, const std::string& ssid, const std::string& password) {
    std::lock_guard<std::mutex> lock(transaction_mutex);
    Journal existing;
    const auto loaded = LoadJournal(existing);
    if (loaded == Load::Unavailable) return CommissioningTransactionResult::RecoveryRequired;
    if (loaded == Load::Loaded) {
        if (existing.generation != generation) return CommissioningTransactionResult::RecoveryRequired;
        return Finish(existing) ? CommissioningTransactionResult::Committed : CommissioningTransactionResult::RecoveryRequired;
    }
    OwnerTrustBundle staged;
    device_foundation::v1::OwnerDomainDescriptor descriptor;
    std::string canonical;
    Journal j;
    j.generation = generation; j.ssid = ssid;
    if (generation == 0 || ssid.empty() || !OwnerTrustStore().LoadStaged(generation, staged) ||
        !ParseOwnerDomainDescriptor(staged.owner_domain_descriptor_json, descriptor, canonical) ||
        descriptor.owner_domain_id != staged.owner_domain_id ||
        !EspIdfCommissioningCredentialStore::GetInstance().StagedDigest(generation, staged.owner_domain_id,
            descriptor.owner_domain_generation, j.identity_digest, j.replacement))
        return CommissioningTransactionResult::SafeFailure;
    // Superseding a legacy interruption is an explicit authenticated setup,
    // never boot-time inference. Its fresh identity must already have a voucher.
    if (loaded == Load::Legacy && !j.replacement) return CommissioningTransactionResult::SafeFailure;
    j.owner = staged.owner_domain_id; j.trust_digest = BundleHash(staged);
    j.network_digest = Hash(std::to_string(ssid.size()) + ":" + ssid + password);
    if (j.trust_digest.empty() || j.network_digest.empty()) return CommissioningTransactionResult::SafeFailure;
    cJSON* network = cJSON_CreateObject();
    if (!network) return CommissioningTransactionResult::SafeFailure;
    cJSON_AddStringToObject(network, "ssid", ssid.c_str());
    cJSON_AddStringToObject(network, "password", password.c_str());
    const auto encoded = Print(network);
    if (encoded.empty() || !Write("wifi", kNetwork, encoded)) return CommissioningTransactionResult::SafeFailure;
    // This durable decision is the point of no return. From here every failure
    // and restart finishes the same transaction, including a torn network write.
    if (!Store(j)) {
        Journal observed;
        return LoadJournal(observed) == Load::Missing
            ? CommissioningTransactionResult::SafeFailure : CommissioningTransactionResult::RecoveryRequired;
    }
    return Finish(j) ? CommissioningTransactionResult::Committed : CommissioningTransactionResult::RecoveryRequired;
}

bool CommissioningTransactionAllowsNewSetup() {
    std::lock_guard<std::mutex> lock(transaction_mutex);
    Journal j;
    const auto loaded = LoadJournal(j);
    return loaded == Load::Missing || loaded == Load::Legacy;
}

bool CommissioningTransactionNeedsFreshIdentity() {
    std::lock_guard<std::mutex> lock(transaction_mutex);
    Journal j;
    return LoadJournal(j) == Load::Legacy;
}

bool RecoverPendingCommissioningTransaction() {
    std::lock_guard<std::mutex> lock(transaction_mutex);
    Journal j;
    const auto loaded = LoadJournal(j);
    return loaded == Load::Missing || (loaded == Load::Loaded && Finish(j));
}

bool RollbackPendingCommissioningTransaction(uint32_t generation) {
    std::lock_guard<std::mutex> lock(transaction_mutex);
    Journal j;
    const auto loaded = LoadJournal(j);
    if (loaded != Load::Missing && loaded != Load::Legacy) return false;
    return EspIdfCommissioningCredentialStore::GetInstance().Rollback(generation) &&
        OwnerTrustStore().RollbackStaged(generation) == OwnerTrustStoreResult::Staged &&
        Erase("wifi", kNetwork);
}
}  // namespace eidolon
