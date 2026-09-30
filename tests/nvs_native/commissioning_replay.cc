// Production transaction/identity code over ESP-IDF's real NVS allocator.
// Hardware flash is replaced with memory; trust/claim ports read real records.
#include <cJSON.h>
#include <openssl/pem.h>
#include <openssl/sha.h>

#include <cassert>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

#include "eidolon/commissioning_transaction.h"
#include "eidolon/device_identity.h"
#include "eidolon/esp_idf_commissioning_credential_store.h"
#include "eidolon/esp_idf_owner_data_erase_storage.h"
#include "eidolon/hub_config_store.h"
#include "eidolon/hub_trust_store.h"
#include "nvs_storage.hpp"

using namespace eidolon;
namespace {
struct PowerLoss {};
int crash_at = -1, writes = 0;
class MemoryPartition : public nvs::Partition {
   public:
    std::vector<uint8_t> data;
    explicit MemoryPartition(size_t size) : data(size, 255) {}
    const char* get_partition_name() override { return "nvs"; }
    uint32_t get_address() override { return 0; }
    uint32_t get_size() override { return data.size(); }
    bool get_readonly() override { return false; }
    esp_err_t read_raw(size_t o, void* p, size_t n) override {
        assert(o + n <= data.size());
        memcpy(p, data.data() + o, n);
        return ESP_OK;
    }
    esp_err_t read(size_t o, void* p, size_t n) override { return read_raw(o, p, n); }
    esp_err_t write_raw(size_t o, const void* p, size_t n) override {
        assert(o + n <= data.size());
        auto b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) {
            assert((data[o + i] & b[i]) == b[i]);
            data[o + i] &= b[i];
        }
        if (++writes == crash_at) throw PowerLoss{};
        return ESP_OK;
    }
    esp_err_t write(size_t o, const void* p, size_t n) override { return write_raw(o, p, n); }
    esp_err_t erase_range(size_t o, size_t n) override {
        assert(o + n <= data.size());
        memset(data.data() + o, 255, n);
        if (++writes == crash_at) throw PowerLoss{};
        return ESP_OK;
    }
};
MemoryPartition flash(0x4000), trust_flash(0x10000);
std::unique_ptr<nvs::Storage> storage, trust_storage;
std::map<nvs_handle_t, uint8_t> handles;
nvs_handle_t next_handle = 1;
void Boot() {
    handles.clear();
    storage.reset();
    trust_storage.reset();
    storage.reset(new (std::nothrow) nvs::Storage(&flash));
    trust_storage.reset(new (std::nothrow) nvs::Storage(&trust_flash));
    assert(storage->init(0, 4) == ESP_OK);
    assert(trust_storage->init(0, 16) == ESP_OK);
    DeviceIdentity::GetInstance().ForgetCachedKeyAfterPhysicalRecovery();
}
std::string Read(nvs::Storage& s, const char* ns, const char* key) {
    uint8_t idx;
    if (s.createOrOpenNamespace(ns, false, idx) != ESP_OK) return {};
    size_t size = 0;
    if (s.getItemDataSize(idx, nvs::ItemType::SZ, key, size) != ESP_OK) return {};
    std::string out(size, '\0');
    assert(s.readItem(idx, nvs::ItemType::SZ, key, out.data(), size) == ESP_OK);
    out.resize(size - 1);
    return out;
}
std::string Read(const char* ns, const char* key) { return Read(*storage, ns, key); }
bool Write(const char* ns, const char* key, const std::string& value) {
    uint8_t idx;
    return storage->createOrOpenNamespace(ns, true, idx) == ESP_OK &&
           storage->writeItem(idx, nvs::ItemType::SZ, key, value.c_str(), value.size() + 1) ==
               ESP_OK;
}
bool Erase(nvs::Storage& s, const char* ns, const char* key) {
    uint8_t idx;
    if (s.createOrOpenNamespace(ns, false, idx) != ESP_OK) return true;
    auto err = s.eraseItem(idx, nvs::ItemType::ANY, key);
    return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
}
std::string Text(cJSON* root, const char* key) {
    auto* x = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(x) ? x->valuestring : "";
}
std::string Hash(const std::string& s) {
    unsigned char h[32];
    SHA256((const unsigned char*)s.data(), s.size(), h);
    std::string out;
    for (auto b : h) {
        out += "0123456789abcdef"[b >> 4];
        out += "0123456789abcdef"[b & 15];
    }
    return out;
}
OwnerTrustBundle LoadTrust(const char* marker) {
    auto slot = Read(*trust_storage, "eidolon", marker);
    OwnerTrustBundle b;
    if (slot != "0" && slot != "1") return b;
    auto get = [&](const char* suffix) {
        return Read(*trust_storage, "eidolon", ("ot" + slot + "_" + suffix).c_str());
    };
    b.owner_domain_id = get("owner");
    b.owner_root_certificate_pem = get("root");
    b.authority_signing_certificate_pem = get("auth");
    b.owner_domain_descriptor_json = get("desc");
    return b;
}
}  // namespace
extern "C" {
esp_err_t nvs_open(const char* ns, nvs_open_mode_t mode, nvs_handle_t* h) {
    uint8_t idx;
    auto err = storage->createOrOpenNamespace(ns, mode == NVS_READWRITE, idx);
    if (err == ESP_OK) {
        *h = next_handle++;
        handles[*h] = idx;
    }
    return err;
}
void nvs_close(nvs_handle_t h) { handles.erase(h); }
esp_err_t nvs_commit(nvs_handle_t) { return ESP_OK; }
esp_err_t nvs_get_str(nvs_handle_t h, const char* k, char* v, size_t* n) {
    size_t size;
    auto err = storage->getItemDataSize(handles.at(h), nvs::ItemType::SZ, k, size);
    if (err != ESP_OK) return err;
    if (!v) {
        *n = size;
        return ESP_OK;
    }
    if (*n < size) return ESP_ERR_NVS_INVALID_LENGTH;
    *n = size;
    return storage->readItem(handles.at(h), nvs::ItemType::SZ, k, v, size);
}
esp_err_t nvs_set_str(nvs_handle_t h, const char* k, const char* v) {
    return storage->writeItem(handles.at(h), nvs::ItemType::SZ, k, v, strlen(v) + 1);
}
esp_err_t nvs_get_u8(nvs_handle_t h, const char* k, uint8_t* v) {
    return storage->readItem(handles.at(h), k, *v);
}
esp_err_t nvs_get_u32(nvs_handle_t h, const char* k, uint32_t* v) {
    return storage->readItem(handles.at(h), k, *v);
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char* k, uint8_t v) {
    return storage->writeItem(handles.at(h), k, v);
}
esp_err_t nvs_set_u32(nvs_handle_t h, const char* k, uint32_t v) {
    return storage->writeItem(handles.at(h), k, v);
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char* k) {
    return storage->eraseItem(handles.at(h), nvs::ItemType::ANY, k);
}
}
namespace eidolon {
bool HubConfigStore::StoreEnrollment(const EnrollmentJournalEntry&) {
    assert(false);
    return false;
}
bool HubConfigStore::ClearEnrollment() { return Erase(*storage, "eidolon", "enrollment"); }
bool HubConfigStore::StoreActiveClaim(const ActiveClaimState&) {
    assert(false);
    return false;
}
OwnerTrustLoadResult OwnerTrustStore::ReadActive(OwnerTrustBundle& b) const {
    return Load(b) ? OwnerTrustLoadResult::Loaded : OwnerTrustLoadResult::NotFound;
}
OwnerTrustStoreResult OwnerTrustStore::Stage(const OwnerTrustBundle&, uint32_t,
                                             const std::function<bool()>&) {
    assert(false);
    return OwnerTrustStoreResult::Unavailable;
}
DeviceIdentity& DeviceIdentity::GetInstance() {
    static DeviceIdentity i;
    return i;
}
void DeviceIdentity::ForgetCachedKeyAfterPhysicalRecovery() {
    private_key_pem_.clear();
    device_instance_id_.clear();
}
bool DeviceIdentity::DescribeKey(const std::string& pem, std::string& key, std::string& instance) {
    BIO* bio = BIO_new_mem_buf(pem.data(), pem.size());
    EVP_PKEY* p = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!p) return false;
    unsigned char* der = nullptr;
    int size = i2d_PUBKEY(p, &der);
    EVP_PKEY_free(p);
    if (size <= 0) return false;
    key = "sha256:" + Hash(std::string((char*)der, size));
    OPENSSL_free(der);
    instance = "device-instance-" + key.substr(7);
    return true;
}
esp_err_t DeviceIdentity::EnsureKeypair() {
    if (EspIdfCommissioningCredentialStore::LoadPrivateKey(private_key_pem_) !=
        CommissioningIdentityLoad::Loaded)
        return ESP_FAIL;
    return DescribeKey(private_key_pem_, fingerprint_, device_instance_id_) ? ESP_OK : ESP_FAIL;
}
bool DeviceIdentity::PrepareKey(bool fresh, std::string& pem, std::string& key,
                                std::string& instance) {
    if (fresh) return false;
    return EspIdfCommissioningCredentialStore::LoadPrivateKey(pem) ==
               CommissioningIdentityLoad::Loaded &&
           DescribeKey(pem, key, instance);
}
ClaimStoreLoadResult HubConfigStore::LoadActiveClaim(ActiveClaimState& out) {
    auto raw = Read("eidolon", "active_claim");
    if (raw.empty()) return ClaimStoreLoadResult::NotFound;
    cJSON* j = cJSON_Parse(raw.c_str());
    if (!j) return ClaimStoreLoadResult::StorageFailure;
    out.device_ref.owner_domain_id.value = Text(j, "owner_domain_id");
    out.device_ref.device_instance_id = Text(j, "device_instance_id");
    auto* gen = cJSON_GetObjectItem(j, "owner_domain_generation");
    out.device_ref.owner_domain_generation = cJSON_IsNumber(gen) ? gen->valuedouble : 0;
    out.state = Text(j, "claim_state") == "active" ? ActiveClaimLocalState::Active
                                                   : ActiveClaimLocalState::Revoked;
    cJSON_Delete(j);
    return ClaimStoreLoadResult::Loaded;
}
ClaimStoreLoadResult HubConfigStore::LoadEnrollment(EnrollmentJournalEntry&) {
    return ClaimStoreLoadResult::NotFound;
}
bool OwnerTrustStore::Load(OwnerTrustBundle& b) const {
    b = LoadTrust("trust_active");
    return !b.owner_domain_id.empty();
}
bool OwnerTrustStore::LoadStaged(uint32_t gen, OwnerTrustBundle& b) const {
    uint8_t ns;
    uint32_t stored = 0;
    if (trust_storage->createOrOpenNamespace("eidolon", false, ns) != ESP_OK ||
        trust_storage->readItem(ns, "trust_sgen", stored) != ESP_OK || gen != stored)
        return false;
    b = LoadTrust("trust_stage");
    return !b.owner_domain_id.empty();
}
OwnerTrustStoreResult OwnerTrustStore::CommitStaged(uint32_t, const std::function<bool()>&) {
    assert(false);
    return OwnerTrustStoreResult::Unavailable;
}
OwnerTrustStoreResult OwnerTrustStore::RollbackStaged(uint32_t) {
    Erase(*trust_storage, "eidolon", "trust_stage");
    Erase(*trust_storage, "eidolon", "trust_sgen");
    return OwnerTrustStoreResult::Staged;
}
bool OwnerTrustStore::DiscardInactive() {
    auto active = Read(*trust_storage, "eidolon", "trust_active");
    auto other = active == "0" ? "1" : "0";
    for (auto suffix : {"owner", "root", "auth", "desc", "digest"})
        if (!Erase(*trust_storage, "eidolon", (std::string("ot") + other + "_" + suffix).c_str()))
            return false;
    return Erase(*trust_storage, "eidolon", "trust_stage") &&
           Erase(*trust_storage, "eidolon", "trust_sgen");
}
bool ParseOwnerDomainDescriptor(const std::string& raw,
                                device_foundation::v1::OwnerDomainDescriptor& out,
                                std::string& canonical) {
    cJSON* j = cJSON_Parse(raw.c_str());
    if (!j) return false;
    out.owner_domain_id = Text(j, "owner_domain_id");
    auto* g = cJSON_GetObjectItem(j, "owner_domain_generation");
    out.owner_domain_generation = cJSON_IsNumber(g) ? g->valuedouble : 0;
    canonical = raw;
    cJSON_Delete(j);
    return out.owner_domain_generation > 0;
}
OwnerDataEraseStorageResult EspIdfOwnerDataEraseStorage::LoadProgress(OwnerDataEraseProgress&) {
    return OwnerDataEraseStorageResult::NotFound;
}
OwnerDataEraseStorageResult EspIdfOwnerDataEraseStorage::StoreProgress(
    const OwnerDataEraseProgress&) {
    return OwnerDataEraseStorageResult::Done;
}
OwnerDataEraseStorageResult EspIdfOwnerDataEraseStorage::EraseTarget(const OwnerDataEraseTarget&) {
    assert(false);
    return OwnerDataEraseStorageResult::RetryableFailure;
}
OwnerDataEraseStorageResult EspIdfOwnerDataEraseStorage::VerifyTargetErased(
    const OwnerDataEraseTarget&) {
    assert(false);
    return OwnerDataEraseStorageResult::RetryableFailure;
}
}  // namespace eidolon
namespace {
std::string Json(cJSON* value) {
    char* raw = cJSON_PrintUnformatted(value);
    assert(raw);
    std::string result = raw;
    cJSON_free(raw);
    cJSON_Delete(value);
    return result;
}
std::string Base64(const std::string& value) {
    std::string out((value.size() + 2) / 3 * 4, '\0');
    EVP_EncodeBlock((unsigned char*)out.data(), (const unsigned char*)value.data(), value.size());
    for (auto& c : out) {
        if (c == '+') c = '-';
        if (c == '/') c = '_';
    }
    while (out.back() == '=') out.pop_back();
    return out;
}
void SeedSynthetic() {
    Boot();
    EVP_PKEY* key = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1");
    assert(key);
    BIO* bio = BIO_new(BIO_s_mem());
    assert(PEM_write_bio_PrivateKey_traditional(bio, key, nullptr, nullptr, 0, nullptr, nullptr) ==
           1);
    char* raw;
    long size = BIO_get_mem_data(bio, &raw);
    std::string pem(raw, size);
    BIO_free(bio);
    EVP_PKEY_free(key);
    std::string key_id, instance;
    assert(DeviceIdentity::DescribeKey(pem, key_id, instance));
    const std::string owner = "owner-domain-test", base = "device-base-" + std::string(64, 'a');
    cJSON* claims = cJSON_CreateObject();
    cJSON_AddStringToObject(claims, "purpose", "eidolon-commissioning-voucher-v1");
    cJSON_AddStringToObject(claims, "owner_domain_id", owner.c_str());
    cJSON_AddStringToObject(claims, "device_base_id", base.c_str());
    cJSON_AddStringToObject(claims, "operational_spki_sha256", key_id.c_str());
    cJSON_AddStringToObject(claims, "jti", "test-voucher");
    cJSON_AddNumberToObject(claims, "exp", 1900000000);
    const std::string voucher = "test." + Base64(Json(claims)) + ".not-a-real-signature";
    auto identity = [&](int generation, bool replacement) {
        cJSON* j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "version", 1);
        cJSON_AddNumberToObject(j, "setup_generation", generation);
        cJSON_AddBoolToObject(j, "replacement", replacement);
        cJSON_AddBoolToObject(j, "ready", true);
        for (auto pair : std::vector<std::pair<const char*, std::string>>{{"key", pem},
                                                                          {"owner", owner},
                                                                          {"owner_generation", "8"},
                                                                          {"key_id", key_id},
                                                                          {"base", base},
                                                                          {"voucher", voucher}})
            cJSON_AddStringToObject(j, pair.first, pair.second.c_str());
        return Json(j);
    };
    const auto pending = identity(1, false);
    assert(Write("eidolon_id", "id_active", identity(2, true)));
    assert(Write("eidolon_id", "id_pending", pending));
    const std::string descriptor = "{\"owner_domain_id\":\"" + owner +
                                   "\",\"owner_domain_generation\":8,\"directory_revision\":1}";
    const std::string root = "test-root", auth = "test-authority";
    uint8_t ns;
    assert(trust_storage->createOrOpenNamespace("eidolon", true, ns) == ESP_OK);
    auto put = [&](const std::string& k, const std::string& v) {
        assert(trust_storage->writeItem(ns, nvs::ItemType::SZ, k.c_str(), v.c_str(),
                                        v.size() + 1) == ESP_OK);
    };
    for (auto slot : {"0", "1"})
        for (auto pair : std::vector<std::pair<std::string, std::string>>{
                 {"owner", owner}, {"root", root}, {"auth", auth}, {"desc", descriptor}})
            put(std::string("ot") + slot + "_" + pair.first, pair.second);
    put("trust_active", "1");
    put("trust_stage", "0");
    assert(trust_storage->writeItem(ns, "trust_sgen", uint32_t(1)) == ESP_OK);
    std::string trust_material;
    for (const auto& field : {owner, root, auth, descriptor})
        trust_material += std::to_string(field.size()) + ":" + field;
    assert(Write("wifi", "ssid", "test-network"));
    assert(Write("wifi", "password", "test-password"));
    assert(Write("wifi", "ctx_candidate",
                 "{\"ssid\":\"test-network\",\"password\":\"test-password\"}"));
    cJSON* journal = cJSON_CreateObject();
    cJSON_AddNumberToObject(journal, "version", 1);
    cJSON_AddNumberToObject(journal, "generation", 1);
    cJSON_AddNumberToObject(journal, "phase", 3);
    cJSON_AddNumberToObject(journal, "cleanup_index", 0);
    cJSON_AddBoolToObject(journal, "replacement", false);
    for (auto pair : std::vector<std::pair<const char*, std::string>>{
             {"owner", owner},
             {"ssid", "test-network"},
             {"network_digest", Hash("12:test-networktest-password")},
             {"trust_digest", Hash(trust_material)},
             {"identity_digest", Hash(pending)}})
        cJSON_AddStringToObject(journal, pair.first, pair.second.c_str());
    assert(Write("eidolon", "ctx_record", Json(journal)));
    assert(Write("eidolon", "active_claim",
                 "{\"owner_domain_id\":\"" + owner +
                     "\",\"owner_domain_generation\":8,\"device_instance_id\":\"" + instance +
                     "\",\"claim_state\":\"active\"}"));
    assert(storage->createOrOpenNamespace("test-fill", true, ns) == ESP_OK);
    int entries = 0;
    while (storage->writeItem(ns, ("p" + std::to_string(entries)).c_str(), uint8_t(1)) == ESP_OK)
        ++entries;
    assert(entries > 12);
    for (int i = entries - 12; i < entries; ++i)
        assert(storage->eraseItem(ns, nvs::ItemType::U8, ("p" + std::to_string(i)).c_str()) ==
               ESP_OK);
    uint8_t idns;
    assert(storage->createOrOpenNamespace("eidolon_id", false, idns) == ESP_OK);
    // Reproduce the old implementation's failure before testing production fix.
    assert(storage->writeItem(idns, nvs::ItemType::SZ, "id_active", pending.c_str(),
                              pending.size() + 1) == ESP_ERR_NVS_NOT_ENOUGH_SPACE);
    std::cout << "synthetic full-partition fixture reproduces old identity-copy failure\n";
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 1)
        SeedSynthetic();
    else {
        assert(argc == 2);
        std::ifstream f(argv[1], std::ios::binary);
        assert(f.good());
        f.seekg(0x1000);
        f.read((char*)flash.data.data(), flash.data.size());
        f.seekg(0x8000);
        f.read((char*)trust_flash.data.data(), trust_flash.data.size());
        assert(f.good());
    }
    const auto original = flash.data, original_trust = trust_flash.data;
    Boot();
    auto& credentials = EspIdfCommissioningCredentialStore::GetInstance();
    CommissioningCredential before;
    assert(credentials.Load(before));
    assert(!Read("eidolon", "ctx_record").empty());
    writes = 0;
    assert(RecoverPendingCommissioningTransaction());
    int total = writes;
    CommissioningCredential after;
    assert(credentials.Load(after));
    assert(before.device_base_id == after.device_base_id &&
           before.operational_key_id == after.operational_key_id);
    assert(Read("eidolon", "ctx_record").empty());
    std::cout << "production Finish recovered original image, identity preserved; flash operations="
              << total << "\n";
    for (int point = 1; point <= total; ++point) {
        flash.data = original;
        trust_flash.data = original_trust;
        Boot();
        writes = 0;
        crash_at = point;
        try {
            RecoverPendingCommissioningTransaction();
        } catch (const PowerLoss&) {
        }
        crash_at = -1;
        Boot();
        assert(RecoverPendingCommissioningTransaction());
        assert(credentials.Load(after));
        assert(before.device_base_id == after.device_base_id &&
               before.operational_key_id == after.operational_key_id);
    }
    std::cout << "every physical flash-write/erase interruption recovered\n";
    // Repeat the complete transaction, including journal/network writes, after
    // reboot resets local generation. NVS page fragmentation is real here.
    for (int i = 0; i < 100; ++i) {
        Boot();
        PreparedCommissioningIdentity prepared;
        assert(credentials.Prepare(after.owner_domain_id, after.owner_domain_generation, 1, false,
                                   prepared));
        assert(!prepared.requires_voucher);
        assert(credentials.Stage(nullptr, 1, [] { return true; }));
        uint8_t ns;
        assert(trust_storage->createOrOpenNamespace("eidolon", false, ns) == ESP_OK);
        const auto active_slot = Read(*trust_storage, "eidolon", "trust_active");
        assert(trust_storage->writeItem(ns, nvs::ItemType::SZ, "trust_stage",
                                       active_slot.c_str(), active_slot.size() + 1) == ESP_OK);
        assert(trust_storage->writeItem(ns, "trust_sgen", uint32_t(1)) == ESP_OK);
        bool replaces = true;
        assert(CommitCommissioningTransaction(1, "maintenance-network",
                                              "password-" + std::to_string(i), &replaces) ==
               CommissioningTransactionResult::Committed);
        assert(!replaces && Read("eidolon", "ctx_record").empty());
        CommissioningCredential current;
        assert(credentials.Load(current));
        assert(current.device_base_id == before.device_base_id &&
               current.operational_key_id == before.operational_key_id);
    }
    std::cout << "100 complete network maintenance transactions across boots passed\n";
    // Exercise legacy key validation over the real NVS allocator with a real
    // PEM parser, not the small key-name adapter used by the transaction test.
    std::string valid_pem;
    assert(EspIdfCommissioningCredentialStore::LoadPrivateKey(valid_pem) ==
           CommissioningIdentityLoad::Loaded);
    flash.data.assign(flash.data.size(), 255);
    trust_flash.data.assign(trust_flash.data.size(), 255);
    Boot();
    std::string loaded_pem;
    assert(EspIdfCommissioningCredentialStore::LoadPrivateKey(loaded_pem) ==
           CommissioningIdentityLoad::NotFound);
    for (const auto& pem : {valid_pem, std::string(""), std::string("not-a-private-key")}) {
        assert(Write("eidolon_id", "p256_priv", pem));
        const auto before_read = flash.data;
        Boot();
        const auto result = EspIdfCommissioningCredentialStore::LoadPrivateKey(loaded_pem);
        assert(result == (pem == valid_pem ? CommissioningIdentityLoad::Loaded
                                         : CommissioningIdentityLoad::Unavailable));
        assert(pem == valid_pem ? loaded_pem == pem : loaded_pem.empty());
        assert(flash.data == before_read); // no replacement, erase or migration
    }
    std::cout << "legacy PEM validation preserves NVS bytes on valid/empty/corrupt keys passed\n";
}
