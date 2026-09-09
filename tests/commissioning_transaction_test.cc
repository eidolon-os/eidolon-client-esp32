#include "eidolon/commissioning_transaction.h"
#include "eidolon/esp_idf_commissioning_credential_store.h"
#include "eidolon/esp_idf_owner_data_erase_storage.h"
#include "eidolon/device_identity.h"
#include "eidolon/hub_trust_store.h"
#include "eidolon/hub_config_store.h"
#include <ssid_manager.h>
#include <cJSON.h>
#include <cassert>
#include <cstring>
#include <map>
#include <stdexcept>
#include <variant>

using namespace eidolon;
namespace {
using Value = std::variant<uint32_t, uint8_t, std::string>;
using Values = std::map<std::string, Value>;
std::map<std::string, Values> disk;
struct Handle { std::string ns; Values pending; };
std::map<nvs_handle_t, Handle> handles;
nvs_handle_t next_handle = 1;
int write_number = 0, crash_at = -1;
bool unavailable = false, fail_trust = false;
int keys_created = 1;
OwnerTrustBundle active_trust, staged_trust;
uint32_t staged_generation = 0;
ActiveClaimState active_claim;
EnrollmentJournalEntry enrollment;
bool has_claim = false, has_enrollment = false;

void PowerLoss() {
    ++write_number;
    if (write_number == crash_at) throw std::runtime_error("power loss");
}
void Reset() {
    disk.clear(); handles.clear(); next_handle = 1;
    write_number = 0; crash_at = -1; unavailable = false; fail_trust = false;
    keys_created = 1; active_trust = {}; staged_trust = {}; staged_generation = 0;
    active_claim = {}; enrollment = {}; has_claim = false; has_enrollment = false;
    SsidManager::GetInstance().RestoreVolatileForTest({});
    DeviceIdentity::GetInstance().ForgetCachedKeyAfterPhysicalRecovery();
    disk["eidolon_id"]["p256_priv"] = std::string("key-1");
}
std::string KeyId(const std::string& pem) {
    return "sha256:" + std::string(64, static_cast<char>('a' + std::stoi(pem.substr(4))));
}
OwnerTrustBundle Bundle(const std::string& owner, const std::string& directory) {
    return {owner, owner + ":" + directory, "root-" + owner, "authority-" + owner};
}
void Prepare(uint32_t generation, const std::string& owner, bool replacement,
             const std::string& directory = "directory-1", uint64_t owner_generation = 1) {
    auto& store = EspIdfCommissioningCredentialStore::GetInstance();
    PreparedCommissioningIdentity identity;
    assert(store.Prepare(owner, owner_generation, generation, replacement, identity));
    CommissioningCredential credential;
    credential.owner_domain_id = owner;
    credential.owner_domain_generation = owner_generation;
    credential.operational_key_id = identity.fingerprint;
    credential.device_base_id = "base-" + identity.device_instance_id;
    // Signature verification remains with Hub. This adapter test exercises the
    // already-validated credential port and the actual NVS candidate storage.
    assert(store.Stage(&credential, generation, [] { return true; }));
    staged_trust = Bundle(owner, directory);
    staged_generation = generation;
}
void SeedOwnerA() {
    Reset();
    Prepare(1, "owner-a", false);
    assert(CommitCommissioningTransaction(1, "network-a", "secret-a") == CommissioningTransactionResult::Committed);
    assert(DeviceIdentity::GetInstance().EnsureKeypair() == ESP_OK);
    active_claim.device_ref.owner_domain_id.value = "owner-a";
    active_claim.device_ref.owner_domain_generation = 1;
    active_claim.device_ref.device_instance_id = DeviceIdentity::GetInstance().DeviceInstanceId();
    active_claim.device_ref.claim_generation = 1;
    active_claim.device_ref.trust_epoch = 1;
    has_claim = true;
}
void AssertOwner(const std::string& owner) {
    CommissioningCredential credential;
    assert(EspIdfCommissioningCredentialStore::GetInstance().Load(credential));
    assert(credential.owner_domain_id == owner);
    assert(active_trust.owner_domain_id == owner);
    assert(DeviceIdentity::GetInstance().EnsureKeypair() == ESP_OK);
    assert(credential.operational_key_id == "sha256:" + DeviceIdentity::GetInstance().DeviceInstanceId().substr(16));
    assert(disk["eidolon"].count("ctx_record") == 0);
    assert(disk["eidolon_id"].count("id_pending") == 0);
    assert(disk["wifi"].count("ctx_candidate") == 0);
}
void SameOwnerUpdatesTheActualDirectoryAndPreservesIdentity() {
    SeedOwnerA();
    const auto old_id = DeviceIdentity::GetInstance().DeviceInstanceId();
    Prepare(2, "owner-a", false, "directory-2");
    assert(CommitCommissioningTransaction(2, "network-a-new", "secret-new") == CommissioningTransactionResult::Committed);
    AssertOwner("owner-a");
    assert(active_trust.owner_domain_descriptor_json == "owner-a:directory-2");
    assert(DeviceIdentity::GetInstance().DeviceInstanceId() == old_id && has_claim);
    assert(SsidManager::GetInstance().GetSsidList().size() == 2);
}
void CancelRestoresTheWholeOldContext() {
    SeedOwnerA();
    const auto before = disk["eidolon_id"].at("id_active");
    Prepare(2, "owner-b", true);
    assert(disk["eidolon_id"].at("id_active") == before);
    assert(RollbackPendingCommissioningTransaction(2));
    AssertOwner("owner-a");
    assert(has_claim && disk["eidolon_id"].at("id_active") == before);
}
void PowerLossWhilePreparingNeverPublishesCredentials() {
    for (int point = 1; point <= 4; ++point) {
        SeedOwnerA();
        const auto old = disk["eidolon_id"].at("id_active");
        write_number = 0; crash_at = point;
        try { Prepare(2, "owner-b", true); }
        catch (const std::runtime_error&) {}
        crash_at = -1; handles.clear();
        assert(RecoverPendingCommissioningTransaction());
        assert(disk["eidolon_id"].at("id_active") == old && has_claim);
        assert(RollbackPendingCommissioningTransaction(2));
        AssertOwner("owner-a");
    }
}
void OwnerChangesAndResetCreateNewLifecycles() {
    SeedOwnerA();
    const auto first = DeviceIdentity::GetInstance().DeviceInstanceId();
    Prepare(2, "owner-b", true);
    assert(CommitCommissioningTransaction(2, "network-b", "secret-b") == CommissioningTransactionResult::Committed);
    AssertOwner("owner-b");
    const auto second = DeviceIdentity::GetInstance().DeviceInstanceId();
    assert(second != first && !has_claim);
    assert(SsidManager::GetInstance().GetSsidList().size() == 1);
    Prepare(3, "owner-a", true);
    assert(CommitCommissioningTransaction(3, "network-a", "new-secret-a") == CommissioningTransactionResult::Committed);
    AssertOwner("owner-a");
    const auto third = DeviceIdentity::GetInstance().DeviceInstanceId();
    assert(third != first && third != second);
    Prepare(4, "owner-a", false, "reset-directory", 2);
    assert(CommitCommissioningTransaction(4, "network-a", "new-secret-a") == CommissioningTransactionResult::Committed);
    AssertOwner("owner-a");
    assert(DeviceIdentity::GetInstance().DeviceInstanceId() != third);
}
void PowerLossAtEveryWriteResumesWithoutOldOwnerRevival() {
    SeedOwnerA(); Prepare(2, "owner-b", true);
    write_number = 0;
    assert(CommitCommissioningTransaction(2, "network-b", "secret-b") == CommissioningTransactionResult::Committed);
    const int total_writes = write_number;
    for (int point = 1; point <= total_writes; ++point) {
        SeedOwnerA(); Prepare(2, "owner-b", true);
        write_number = 0; crash_at = point;
        try { CommitCommissioningTransaction(2, "network-b", "secret-b"); }
        catch (const std::runtime_error&) {}
        crash_at = -1; handles.clear();
        std::vector<SsidItem> saved;
        for (int i = 0; i < 10; ++i) {
            const auto suffix = i == 0 ? std::string{} : std::to_string(i);
            const auto& wifi = disk["wifi"];
            if (wifi.count("ssid" + suffix) && wifi.count("password" + suffix)) {
                saved.push_back({std::get<std::string>(wifi.at("ssid" + suffix)),
                                 std::get<std::string>(wifi.at("password" + suffix))});
            }
        }
        SsidManager::GetInstance().RestoreVolatileForTest(saved);
        DeviceIdentity::GetInstance().ForgetCachedKeyAfterPhysicalRecovery();
        const bool decided = disk["eidolon"].count("ctx_record") != 0;
        assert(RecoverPendingCommissioningTransaction());
        if (decided || active_trust.owner_domain_id == "owner-b") {
            AssertOwner("owner-b");
            assert(!has_claim);
        } else {
            assert(RollbackPendingCommissioningTransaction(2));
            AssertOwner("owner-a");
            assert(has_claim);
        }
    }
}
void CommitFailureCannotBeCancelledOrOverwritten() {
    SeedOwnerA(); Prepare(2, "owner-b", true);
    fail_trust = true;
    assert(CommitCommissioningTransaction(2, "network-b", "secret-b") == CommissioningTransactionResult::RecoveryRequired);
    assert(!RollbackPendingCommissioningTransaction(2));
    assert(CommitCommissioningTransaction(3, "wrong", "wrong") == CommissioningTransactionResult::RecoveryRequired);
    assert(!RecoverPendingCommissioningTransaction());
    fail_trust = false;
    assert(RecoverPendingCommissioningTransaction());
    AssertOwner("owner-b");
}
void LegacyInterruptionNeedsAnAuthorizedFreshLifecycle() {
    SeedOwnerA();
    const auto old_id = DeviceIdentity::GetInstance().DeviceInstanceId();
    disk["eidolon"]["ctx_gen"] = uint32_t(8);
    assert(!RecoverPendingCommissioningTransaction());
    assert(CommissioningTransactionAllowsNewSetup());
    Prepare(9, "owner-a", false);
    assert(CommitCommissioningTransaction(9, "network-a", "secret-a") == CommissioningTransactionResult::Committed);
    AssertOwner("owner-a");
    assert(DeviceIdentity::GetInstance().DeviceInstanceId() != old_id);
    assert(!has_claim && disk["eidolon"].count("ctx_gen") == 0);
}
void CorruptionAndUnavailableStorageFailClosed() {
    Reset(); unavailable = true;
    assert(!RecoverPendingCommissioningTransaction());
    unavailable = false;
    disk["eidolon"]["ctx_record"] = std::string("{\"phase\":2}");
    assert(!RecoverPendingCommissioningTransaction());
    assert(!RollbackPendingCommissioningTransaction(2));
    Reset(); disk["eidolon"]["ctx_gen"] = uint32_t(2);
    assert(!RecoverPendingCommissioningTransaction());
    Reset(); disk["eidolon_id"]["id_active"] = std::string("broken");
    std::string pem;
    assert(EspIdfCommissioningCredentialStore::LoadPrivateKey(pem) == CommissioningIdentityLoad::Unavailable);
}
}

esp_err_t nvs_open(const char* ns, int, nvs_handle_t* handle) {
    if (unavailable) return ESP_FAIL;
    *handle = next_handle++;
    handles[*handle] = {ns, disk[ns]};
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { handles.erase(handle); }
template<class T> esp_err_t Get(nvs_handle_t handle, const char* key, T* value) {
    const auto& values = disk[handles.at(handle).ns];
    const auto found = values.find(key);
    if (found == values.end()) return ESP_ERR_NVS_NOT_FOUND;
    if (!std::holds_alternative<T>(found->second)) return ESP_FAIL;
    *value = std::get<T>(found->second); return ESP_OK;
}
esp_err_t nvs_get_u32(nvs_handle_t h, const char* k, uint32_t* v) { return Get(h,k,v); }
esp_err_t nvs_get_u8(nvs_handle_t h, const char* k, uint8_t* v) { return Get(h,k,v); }
esp_err_t nvs_get_str(nvs_handle_t h, const char* k, char* v, size_t* size) {
    std::string value;
    const auto err = Get(h,k,&value);
    if (err != ESP_OK) return err;
    if (!v) { *size = value.size()+1; return ESP_OK; }
    if (*size < value.size()+1) return ESP_FAIL;
    std::memcpy(v,value.c_str(),value.size()+1); *size=value.size()+1; return ESP_OK;
}
// Model NVS item writes as durable individually, rather than pretending that
// nvs_commit makes unrelated keys atomic. Every item is a crash boundary.
esp_err_t nvs_set_str(nvs_handle_t h,const char* k,const char* v) { disk[handles.at(h).ns][k]=std::string(v); PowerLoss(); return ESP_OK; }
esp_err_t nvs_set_u32(nvs_handle_t h,const char* k,uint32_t v) { disk[handles.at(h).ns][k]=v; PowerLoss(); return ESP_OK; }
esp_err_t nvs_set_u8(nvs_handle_t h,const char* k,uint8_t v) { disk[handles.at(h).ns][k]=v; PowerLoss(); return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t h,const char* k) {
    if (!disk[handles.at(h).ns].erase(k)) return ESP_ERR_NVS_NOT_FOUND;
    PowerLoss(); return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t) { PowerLoss(); return ESP_OK; }


namespace eidolon {
bool ParseOwnerDomainDescriptor(const std::string& raw,
    device_foundation::v1::OwnerDomainDescriptor& out, std::string& canonical) {
    out.owner_domain_id = raw.substr(0, raw.find(':'));
    out.owner_domain_generation = raw.find("reset-directory") != std::string::npos ? 2 : 1;
    canonical = raw;
    return true;
}
DeviceIdentity& DeviceIdentity::GetInstance() { static DeviceIdentity instance; return instance; }
void DeviceIdentity::ForgetCachedKeyAfterPhysicalRecovery() { private_key_pem_.clear(); device_instance_id_.clear(); }
esp_err_t DeviceIdentity::EnsureKeypair() {
    if (!device_instance_id_.empty()) return ESP_OK;
    if (EspIdfCommissioningCredentialStore::LoadPrivateKey(private_key_pem_) != CommissioningIdentityLoad::Loaded) return ESP_FAIL;
    return DescribeKey(private_key_pem_,fingerprint_,device_instance_id_) ? ESP_OK : ESP_FAIL;
}
bool DeviceIdentity::DescribeKey(const std::string& pem, std::string& key, std::string& instance) {
    key=KeyId(pem); instance="device-instance-"+key.substr(7); return true;
}
bool DeviceIdentity::PrepareKey(bool fresh,std::string& pem,std::string& key,std::string& instance) {
    if (fresh) pem="key-"+std::to_string(++keys_created);
    else if (EspIdfCommissioningCredentialStore::LoadPrivateKey(pem) != CommissioningIdentityLoad::Loaded) return false;
    return DescribeKey(pem,key,instance);
}
OwnerTrustLoadResult OwnerTrustStore::ReadActive(OwnerTrustBundle& out) const {
    out=active_trust; return out.owner_domain_id.empty()?OwnerTrustLoadResult::NotFound:OwnerTrustLoadResult::Loaded;
}
bool OwnerTrustStore::Load(OwnerTrustBundle& out) const { return ReadActive(out)==OwnerTrustLoadResult::Loaded; }
bool OwnerTrustStore::LoadStaged(uint32_t generation,OwnerTrustBundle& out) const {
    out=staged_trust; return generation==staged_generation && !out.owner_domain_id.empty();
}
OwnerTrustStoreResult OwnerTrustStore::Stage(const OwnerTrustBundle& value,uint32_t generation,const std::function<bool()>& guard) {
    if(!guard()) return OwnerTrustStoreResult::Stale;
    staged_trust=value; staged_generation=generation; return OwnerTrustStoreResult::Staged;
}
OwnerTrustStoreResult OwnerTrustStore::CommitStaged(uint32_t generation,const std::function<bool()>& guard) {
    if(fail_trust) return OwnerTrustStoreResult::Unavailable;
    if(!guard() || generation!=staged_generation) return OwnerTrustStoreResult::Stale;
    active_trust=staged_trust; staged_generation=0; PowerLoss(); return OwnerTrustStoreResult::Staged;
}
OwnerTrustStoreResult OwnerTrustStore::RollbackStaged(uint32_t generation) {
    if(staged_generation && generation!=staged_generation) return OwnerTrustStoreResult::Stale;
    staged_trust={}; staged_generation=0; return OwnerTrustStoreResult::Staged;
}
bool OwnerTrustStore::DiscardInactive() { staged_trust={}; staged_generation=0; PowerLoss(); return true; }
ClaimStoreLoadResult HubConfigStore::LoadActiveClaim(ActiveClaimState& out) { out=active_claim; return has_claim?ClaimStoreLoadResult::Loaded:ClaimStoreLoadResult::NotFound; }
ClaimStoreLoadResult HubConfigStore::LoadEnrollment(EnrollmentJournalEntry& out) { out=enrollment; return has_enrollment?ClaimStoreLoadResult::Loaded:ClaimStoreLoadResult::NotFound; }
bool HubConfigStore::StoreEnrollment(const EnrollmentJournalEntry& value) { enrollment=value; has_enrollment=true; return true; }
bool HubConfigStore::ClearEnrollment() { has_enrollment=false; return true; }
bool HubConfigStore::StoreActiveClaim(const ActiveClaimState& value) { active_claim=value; has_claim=true; return true; }
OwnerDataEraseStorageResult EspIdfOwnerDataEraseStorage::LoadProgress(OwnerDataEraseProgress&) { return OwnerDataEraseStorageResult::NotFound; }
OwnerDataEraseStorageResult EspIdfOwnerDataEraseStorage::StoreProgress(const OwnerDataEraseProgress&) { return OwnerDataEraseStorageResult::Done; }
OwnerDataEraseStorageResult EspIdfOwnerDataEraseStorage::EraseTarget(const OwnerDataEraseTarget& target) {
    if (target.target_id=="active-claim-and-onboarding") { has_claim=false; has_enrollment=false; }
    for(const auto& key:target.keys) disk[target.nvs_namespace].erase(key);
    PowerLoss(); return OwnerDataEraseStorageResult::Done;
}
OwnerDataEraseStorageResult EspIdfOwnerDataEraseStorage::VerifyTargetErased(const OwnerDataEraseTarget& target) {
    for(const auto& key:target.keys) if(disk[target.nvs_namespace].count(key)) return OwnerDataEraseStorageResult::RetryableFailure;
    return OwnerDataEraseStorageResult::Done;
}
}
int main() {
    PowerLossWhilePreparingNeverPublishesCredentials();
    LegacyInterruptionNeedsAnAuthorizedFreshLifecycle();
    SameOwnerUpdatesTheActualDirectoryAndPreservesIdentity();
    CancelRestoresTheWholeOldContext();
    OwnerChangesAndResetCreateNewLifecycles();
    PowerLossAtEveryWriteResumesWithoutOldOwnerRevival();
    CommitFailureCannotBeCancelledOrOverwritten();
    CorruptionAndUnavailableStorageFailClosed();
}
