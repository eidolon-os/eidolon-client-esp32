#include "eidolon/commissioning_transaction.h"

#include "eidolon/hub_trust_store.h"

#include <nvs.h>
#include <ssid_manager.h>

#include <cassert>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <variant>

namespace {

using Value = std::variant<std::uint32_t, std::uint8_t, std::string>;

std::map<std::string, Value> committed;
std::map<std::string, Value> pending;
bool writable = false;

eidolon::OwnerTrustBundle staged;
eidolon::OwnerTrustBundle active;
std::uint32_t staged_generation = 0;

void Reset()
{
    committed.clear();
    pending.clear();
    writable = false;
    staged = {};
    active = {};
    staged_generation = 0;
    SsidManager::GetInstance().Clear();
}

void Stage(std::uint32_t generation, const std::string& owner = "owner-domain_01")
{
    staged = {
        .owner_domain_id = owner,
        .owner_domain_descriptor_json = "descriptor",
        .owner_root_certificate_pem = "root",
        .authority_signing_certificate_pem = "authority",
    };
    staged_generation = generation;
}

void SeedJournal(std::uint32_t generation, std::uint8_t phase,
                 const std::string& ssid, const std::string& digest)
{
    committed["ctx_gen"] = generation;
    committed["ctx_phase"] = phase;
    committed["ctx_owner"] = std::string("owner-domain_01");
    committed["ctx_ssid"] = ssid;
    committed["ctx_digest"] = digest;
}

std::string DigestAfterSuccessfulCommit(const std::string& ssid,
                                        const std::string& password)
{
    Reset();
    Stage(7);
    assert(eidolon::CommitCommissioningTransaction(7, ssid, password) ==
           eidolon::CommissioningTransactionResult::Committed);
    const auto digest = std::get<std::string>(committed.at("last_digest"));
    Reset();
    return digest;
}

void SuccessfulCommitStoresNoReplayablePassword()
{
    Reset();
    Stage(7);

    assert(eidolon::CommitCommissioningTransaction(7, "home", "secret") ==
           eidolon::CommissioningTransactionResult::Committed);
    assert(active.owner_domain_id == "owner-domain_01");
    assert(staged_generation == 0);
    assert(committed.count("ctx_gen") == 0);
    const auto& profiles = SsidManager::GetInstance().GetSsidList();
    assert(profiles.size() == 1);
    assert(profiles[0].ssid == "home");
    assert(profiles[0].password == "secret");
}

void PreparedWithoutDurableNetworkRollsBack()
{
    const auto digest = DigestAfterSuccessfulCommit("new-home", "new-secret");
    Stage(8);
    SeedJournal(8, 1, "new-home", digest);

    assert(eidolon::RecoverPendingCommissioningTransaction());
    assert(staged_generation == 0);
    assert(active.owner_domain_id.empty());
    assert(committed.count("ctx_gen") == 0);
}

void PreparedWithDurableNetworkCrashForwards()
{
    const auto digest = DigestAfterSuccessfulCommit("new-home", "new-secret");
    Stage(8);
    SsidManager::GetInstance().AddSsid("new-home", "new-secret");
    SeedJournal(8, 1, "new-home", digest);

    assert(eidolon::RecoverPendingCommissioningTransaction());
    assert(active.owner_domain_id == "owner-domain_01");
    assert(staged_generation == 0);
    assert(committed.count("ctx_gen") == 0);
}

void NetworkCommittedCrashForwardsTrust()
{
    const auto digest = DigestAfterSuccessfulCommit("new-home", "new-secret");
    Stage(8);
    SsidManager::GetInstance().AddSsid("new-home", "new-secret");
    SeedJournal(8, 2, "new-home", digest);

    assert(eidolon::RecoverPendingCommissioningTransaction());
    assert(active.owner_domain_id == "owner-domain_01");
    assert(committed.count("ctx_gen") == 0);
}

void TrustCommittedOnlyClearsTheJournal()
{
    const auto digest = DigestAfterSuccessfulCommit("new-home", "new-secret");
    active.owner_domain_id = "owner-domain_01";
    SeedJournal(8, 3, "new-home", digest);

    assert(eidolon::RecoverPendingCommissioningTransaction());
    assert(active.owner_domain_id == "owner-domain_01");
    assert(committed.count("ctx_gen") == 0);
}

void AGenerationCannotRollbackAnotherTransaction()
{
    const auto digest = DigestAfterSuccessfulCommit("new-home", "new-secret");
    Stage(8);
    SeedJournal(8, 1, "new-home", digest);

    assert(!eidolon::RollbackPendingCommissioningTransaction(9));
    assert(staged_generation == 8);
    assert(committed.count("ctx_gen") == 1);
}

}  // namespace

esp_err_t nvs_open(const char*, int mode, nvs_handle_t* handle)
{
    *handle = 1;
    writable = mode == NVS_READWRITE;
    pending = committed;
    return ESP_OK;
}

void nvs_close(nvs_handle_t)
{
    pending.clear();
    writable = false;
}

template <typename T>
esp_err_t Read(const char* key, T* value)
{
    const auto found = committed.find(key);
    if (found == committed.end() || !std::holds_alternative<T>(found->second)) {
        return ESP_FAIL;
    }
    *value = std::get<T>(found->second);
    return ESP_OK;
}

esp_err_t nvs_get_u32(nvs_handle_t, const char* key, std::uint32_t* value)
{
    return Read(key, value);
}

esp_err_t nvs_get_u8(nvs_handle_t, const char* key, std::uint8_t* value)
{
    return Read(key, value);
}

esp_err_t nvs_get_str(nvs_handle_t, const char* key, char* value,
                      std::size_t* length)
{
    const auto found = committed.find(key);
    if (found == committed.end() ||
        !std::holds_alternative<std::string>(found->second)) return ESP_FAIL;
    const auto& text = std::get<std::string>(found->second);
    const std::size_t required = text.size() + 1;
    if (value == nullptr) {
        *length = required;
        return ESP_OK;
    }
    if (*length < required) return ESP_FAIL;
    std::memcpy(value, text.c_str(), required);
    *length = required;
    return ESP_OK;
}

template <typename T>
esp_err_t Write(const char* key, T value)
{
    if (!writable) return ESP_FAIL;
    pending[key] = std::move(value);
    return ESP_OK;
}

esp_err_t nvs_set_u32(nvs_handle_t, const char* key, std::uint32_t value)
{
    return Write(key, value);
}

esp_err_t nvs_set_u8(nvs_handle_t, const char* key, std::uint8_t value)
{
    return Write(key, value);
}

esp_err_t nvs_set_str(nvs_handle_t, const char* key, const char* value)
{
    if (std::string(key) == "ctx_digest") pending["last_digest"] = std::string(value);
    return Write(key, std::string(value));
}

esp_err_t nvs_erase_key(nvs_handle_t, const char* key)
{
    if (!writable || pending.erase(key) == 0) return ESP_FAIL;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t)
{
    if (!writable) return ESP_FAIL;
    committed = pending;
    return ESP_OK;
}

namespace eidolon {

OwnerTrustStoreResult OwnerTrustStore::Stage(
    const OwnerTrustBundle& bundle, std::uint32_t generation,
    const std::function<bool()>& guard)
{
    if (!guard()) return OwnerTrustStoreResult::Stale;
    staged = bundle;
    staged_generation = generation;
    return OwnerTrustStoreResult::Staged;
}

bool OwnerTrustStore::LoadStaged(std::uint32_t generation,
                                 OwnerTrustBundle& bundle) const
{
    if (generation != staged_generation || staged.owner_domain_id.empty()) return false;
    bundle = staged;
    return true;
}

OwnerTrustStoreResult OwnerTrustStore::CommitStaged(
    std::uint32_t generation, const std::function<bool()>& guard)
{
    if (!guard() || generation != staged_generation) return OwnerTrustStoreResult::Stale;
    active = staged;
    staged = {};
    staged_generation = 0;
    return OwnerTrustStoreResult::Staged;
}

OwnerTrustStoreResult OwnerTrustStore::RollbackStaged(std::uint32_t generation)
{
    if (generation != staged_generation) return OwnerTrustStoreResult::Stale;
    staged = {};
    staged_generation = 0;
    return OwnerTrustStoreResult::Staged;
}

bool OwnerTrustStore::Load(OwnerTrustBundle& bundle) const
{
    if (active.owner_domain_id.empty()) return false;
    bundle = active;
    return true;
}

}  // namespace eidolon

int main()
{
    SuccessfulCommitStoresNoReplayablePassword();
    PreparedWithoutDurableNetworkRollsBack();
    PreparedWithDurableNetworkCrashForwards();
    NetworkCommittedCrashForwardsTrust();
    TrustCommittedOnlyClearsTheJournal();
    AGenerationCannotRollbackAnotherTransaction();
    return 0;
}
