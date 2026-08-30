#include "commissioning_transaction.h"
#include "mbedtls_compat.h"

#include "hub_trust_store.h"
#include "hub_types.h"

#include <ssid_manager.h>

#include <esp_log.h>
#include <nvs.h>

#include <array>

#define TAG "CommissioningTxn"

namespace eidolon {
namespace {

constexpr const char* kGeneration = "ctx_gen";
constexpr const char* kPhase = "ctx_phase";
constexpr const char* kOwner = "ctx_owner";
constexpr const char* kSsid = "ctx_ssid";
constexpr const char* kDigest = "ctx_digest";
constexpr uint8_t kPrepared = 1;
constexpr uint8_t kNetworkCommitted = 2;
constexpr uint8_t kTrustCommitted = 3;

struct Journal {
    uint32_t generation = 0;
    uint8_t phase = 0;
    std::string owner;
    std::string ssid;
    std::string digest;
};

std::string ReadString(nvs_handle_t handle, const char* key)
{
    size_t size = 0;
    if (nvs_get_str(handle, key, nullptr, &size) != ESP_OK || size == 0) return {};
    std::string value(size, '\0');
    if (nvs_get_str(handle, key, value.data(), &size) != ESP_OK) return {};
    while (!value.empty() && value.back() == '\0') value.pop_back();
    return value;
}

std::string CredentialDigest(const std::string& ssid,
                             const std::string& password)
{
    std::string material = std::to_string(ssid.size()) + ":" + ssid + "\0" +
                           std::to_string(password.size()) + ":" + password;
    std::array<unsigned char, 32> digest{};
    if (mbedtls_sha256(
            reinterpret_cast<const unsigned char*>(material.data()),
            material.size(), digest.data(), 0) != 0) return {};
    static constexpr char hex[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(64);
    for (const auto byte : digest) {
        encoded.push_back(hex[byte >> 4]);
        encoded.push_back(hex[byte & 0x0f]);
    }
    return encoded;
}

bool LoadJournal(Journal& journal)
{
    journal = {};
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
    const bool loaded =
        nvs_get_u32(handle, kGeneration, &journal.generation) == ESP_OK &&
        nvs_get_u8(handle, kPhase, &journal.phase) == ESP_OK;
    if (loaded) {
        journal.owner = ReadString(handle, kOwner);
        journal.ssid = ReadString(handle, kSsid);
        journal.digest = ReadString(handle, kDigest);
    }
    nvs_close(handle);
    return loaded && journal.generation != 0 && !journal.owner.empty() &&
           !journal.ssid.empty() && journal.digest.size() == 64 &&
           journal.phase >= kPrepared && journal.phase <= kTrustCommitted;
}

bool WritePrepared(const Journal& journal)
{
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t result = nvs_set_u32(handle, kGeneration, journal.generation);
    if (result == ESP_OK) result = nvs_set_u8(handle, kPhase, kPrepared);
    if (result == ESP_OK) result = nvs_set_str(handle, kOwner, journal.owner.c_str());
    if (result == ESP_OK) result = nvs_set_str(handle, kSsid, journal.ssid.c_str());
    if (result == ESP_OK) result = nvs_set_str(handle, kDigest, journal.digest.c_str());
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}

bool WritePhase(uint8_t phase)
{
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t result = nvs_set_u8(handle, kPhase, phase);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}

bool ClearJournal()
{
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t result = nvs_erase_key(handle, kGeneration);
    if (result == ESP_OK) result = nvs_erase_key(handle, kPhase);
    if (result == ESP_OK) result = nvs_erase_key(handle, kOwner);
    if (result == ESP_OK) result = nvs_erase_key(handle, kSsid);
    if (result == ESP_OK) result = nvs_erase_key(handle, kDigest);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}

bool ProfileMatches(const Journal& journal)
{
    for (const auto& item : SsidManager::GetInstance().GetSsidList()) {
        if (item.ssid == journal.ssid &&
            CredentialDigest(item.ssid, item.password) == journal.digest) return true;
    }
    return false;
}

bool ActiveOwnerMatches(const std::string& owner)
{
    OwnerTrustBundle active;
    return OwnerTrustStore().Load(active) && active.owner_domain_id == owner;
}

bool FinishTrust(const Journal& journal)
{
    if (!ActiveOwnerMatches(journal.owner)) {
        const auto guard = [] { return true; };
        if (OwnerTrustStore().CommitStaged(journal.generation, guard) !=
                OwnerTrustStoreResult::Staged ||
            !ActiveOwnerMatches(journal.owner)) return false;
    }
    return WritePhase(kTrustCommitted) && ClearJournal();
}

}  // namespace

CommissioningTransactionResult CommitCommissioningTransaction(
    uint32_t generation, const std::string& ssid, const std::string& password)
{
    OwnerTrustBundle staged;
    Journal journal;
    journal.generation = generation;
    journal.ssid = ssid;
    journal.digest = CredentialDigest(ssid, password);
    if (generation == 0 || ssid.empty() || journal.digest.empty() ||
        !OwnerTrustStore().LoadStaged(generation, staged)) {
        return CommissioningTransactionResult::SafeFailure;
    }
    journal.owner = staged.owner_domain_id;
    if (!WritePrepared(journal)) return CommissioningTransactionResult::SafeFailure;

    SsidManager::GetInstance().AddSsid(ssid, password);
    if (!ProfileMatches(journal)) {
        OwnerTrustStore().RollbackStaged(generation);
        ClearJournal();
        return CommissioningTransactionResult::SafeFailure;
    }
    if (!WritePhase(kNetworkCommitted)) {
        return CommissioningTransactionResult::RecoveryRequired;
    }
    return FinishTrust(journal) ? CommissioningTransactionResult::Committed
                                : CommissioningTransactionResult::RecoveryRequired;
}

bool RecoverPendingCommissioningTransaction()
{
    Journal journal;
    if (!LoadJournal(journal)) return true;
    if (journal.phase == kPrepared && !ProfileMatches(journal)) {
        OwnerTrustStore().RollbackStaged(journal.generation);
        return ClearJournal();
    }
    if (journal.phase == kPrepared && !WritePhase(kNetworkCommitted)) return false;
    if (journal.phase == kPrepared || journal.phase == kNetworkCommitted) {
        return FinishTrust(journal);
    }
    return ActiveOwnerMatches(journal.owner) && ClearJournal();
}

bool RollbackPendingCommissioningTransaction(uint32_t generation)
{
    Journal journal;
    if (LoadJournal(journal)) {
        if (journal.generation != generation || journal.phase != kPrepared ||
            ProfileMatches(journal)) return false;
        if (!ClearJournal()) return false;
    }
    return OwnerTrustStore().RollbackStaged(generation) ==
           OwnerTrustStoreResult::Staged;
}

}  // namespace eidolon
