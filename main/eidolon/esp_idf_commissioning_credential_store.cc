#include "esp_idf_commissioning_credential_store.h"
#include "commissioning_transaction.h"

#include "device_identity.h"
#include "hub_config_store.h"
#include "mbedtls_sha256_compat.h"

#include <cJSON.h>
#include <nvs.h>
#include <array>
#include <cstdlib>
#include <cerrno>
#include <mutex>

namespace eidolon {
namespace {
std::recursive_mutex mutex;
constexpr const char* kActive = "id_active";
constexpr const char* kPending = "id_pending";
using Load = CommissioningIdentityLoad;

struct Identity {
    uint32_t generation = 0;
    bool replacement = false;
    bool ready = false;
    std::string pem;
    CommissioningCredential credential;
};

Load ReadString(const char* key, std::string& out) {
    out.clear();
    nvs_handle_t handle = 0;
    auto err = nvs_open(kCommissioningCredentialNamespace, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return Load::NotFound;
    if (err != ESP_OK) return Load::Unavailable;
    size_t size = 0;
    err = nvs_get_str(handle, key, nullptr, &size);
    if (err == ESP_OK && size > 0 && size < 16384) {
        out.resize(size);
        err = nvs_get_str(handle, key, out.data(), &size);
        if (err == ESP_OK) out.resize(size - 1);
    } else if (err == ESP_OK) err = ESP_FAIL;
    nvs_close(handle);
    return err == ESP_OK ? Load::Loaded :
           err == ESP_ERR_NVS_NOT_FOUND ? Load::NotFound : Load::Unavailable;
}

bool WriteString(const char* key, const std::string& value) {
    nvs_handle_t handle = 0;
    if (nvs_open(kCommissioningCredentialNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    auto err = nvs_set_str(handle, key, value.c_str());
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

bool Erase(const std::vector<const char*>& keys) {
    nvs_handle_t handle = 0;
    auto err = nvs_open(kCommissioningCredentialNamespace, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;
    if (err != ESP_OK) return false;
    for (const auto* key : keys) {
        err = nvs_erase_key(handle, key);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) break;
        err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

std::string Text(const cJSON* root, const char* key) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

std::string Encode(const Identity& value) {
    cJSON* root = cJSON_CreateObject();
    if (!root) return {};
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddNumberToObject(root, "setup_generation", value.generation);
    cJSON_AddBoolToObject(root, "replacement", value.replacement);
    cJSON_AddBoolToObject(root, "ready", value.ready);
    cJSON_AddStringToObject(root, "key", value.pem.c_str());
    const auto& c = value.credential;
    cJSON_AddStringToObject(root, "owner", c.owner_domain_id.c_str());
    cJSON_AddStringToObject(root, "owner_generation", std::to_string(c.owner_domain_generation).c_str());
    cJSON_AddStringToObject(root, "key_id", c.operational_key_id.c_str());
    cJSON_AddStringToObject(root, "base", c.device_base_id.c_str());
    cJSON_AddStringToObject(root, "voucher", c.voucher.c_str());
    char* raw = cJSON_PrintUnformatted(root);
    std::string result = raw ? raw : "";
    cJSON_free(raw);
    cJSON_Delete(root);
    return result;
}

bool Decode(const std::string& raw, Identity& out) {
    cJSON* root = cJSON_ParseWithLength(raw.data(), raw.size());
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return false; }
    const auto* version = cJSON_GetObjectItemCaseSensitive(root, "version");
    const auto* generation = cJSON_GetObjectItemCaseSensitive(root, "setup_generation");
    const auto* replacement = cJSON_GetObjectItemCaseSensitive(root, "replacement");
    const auto* ready = cJSON_GetObjectItemCaseSensitive(root, "ready");
    Identity value;
    value.pem = Text(root, "key");
    value.credential.owner_domain_id = Text(root, "owner");
    const auto owner_generation = Text(root, "owner_generation");
    char* end = nullptr;
    errno = 0;
    value.credential.owner_domain_generation = std::strtoull(owner_generation.c_str(), &end, 10);
    bool valid = cJSON_IsNumber(version) && version->valuedouble == 1 &&
        cJSON_IsNumber(generation) && generation->valuedouble > 0 &&
        generation->valuedouble <= UINT32_MAX &&
        cJSON_IsBool(replacement) && cJSON_IsBool(ready) &&
        !owner_generation.empty() && owner_generation.find_first_not_of("0123456789") == std::string::npos &&
        errno != ERANGE && end && *end == '\0' &&
        value.credential.owner_domain_generation != 0 &&
        !value.pem.empty() && !value.credential.owner_domain_id.empty();
    if (valid) {
        value.generation = static_cast<uint32_t>(generation->valuedouble);
        valid = value.generation == generation->valuedouble;
    }
    value.replacement = cJSON_IsTrue(replacement);
    value.ready = cJSON_IsTrue(ready);
    value.credential.operational_key_id = Text(root, "key_id");
    value.credential.device_base_id = Text(root, "base");
    value.credential.voucher = Text(root, "voucher");
    if (!value.credential.voucher.empty()) {
        CommissioningCredential parsed;
        valid = valid && ParseCommissioningVoucher(value.credential.voucher, parsed) &&
            parsed.owner_domain_id == value.credential.owner_domain_id &&
            parsed.operational_key_id == value.credential.operational_key_id &&
            parsed.device_base_id == value.credential.device_base_id;
        value.credential.voucher_jti = parsed.voucher_jti;
        value.credential.voucher_expires_at_unix = parsed.voucher_expires_at_unix;
    }
    valid = valid && value.credential.operational_key_id.size() == 71 &&
        (!value.ready || !value.credential.device_base_id.empty());
    cJSON_Delete(root);
    if (valid) out = std::move(value);
    return valid;
}

Load ReadIdentity(const char* key, Identity& out) {
    std::string raw;
    const auto loaded = ReadString(key, raw);
    if (loaded != Load::Loaded) return loaded;
    return Decode(raw, out) ? Load::Loaded : Load::Unavailable;
}

std::string Digest(const std::string& raw) {
    std::array<unsigned char, 32> bytes{};
    if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(raw.data()), raw.size(), bytes.data(), 0) != 0) return {};
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (const auto byte : bytes) { out += hex[byte >> 4]; out += hex[byte & 15]; }
    return out;
}
}

EspIdfCommissioningCredentialStore& EspIdfCommissioningCredentialStore::GetInstance() {
    static EspIdfCommissioningCredentialStore store;
    return store;
}

CommissioningIdentityLoad EspIdfCommissioningCredentialStore::LoadPrivateKey(std::string& pem) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    Identity active;
    const auto loaded = ReadIdentity(kActive, active);
    if (loaded == Load::Loaded) {
        std::string key_id, instance;
        if (!active.ready || !DeviceIdentity::DescribeKey(active.pem, key_id, instance) ||
            key_id != active.credential.operational_key_id) return Load::Unavailable;
        pem = active.pem;
        return loaded;
    }
    if (loaded == Load::Unavailable) return loaded;
    const auto legacy = ReadString("p256_priv", pem);
    if (legacy == Load::NotFound) {
        std::string base;
        if (ReadString("base_id", base) != Load::NotFound) return Load::Unavailable;
    }
    return legacy;
}

bool EspIdfCommissioningCredentialStore::Load(CommissioningCredential& out) const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    out = {};
    Identity active;
    const auto loaded = ReadIdentity(kActive, active);
    if (loaded == Load::Loaded) { out = active.credential; return active.ready; }
    if (loaded == Load::Unavailable) return false;
    if (ReadString("base_id", out.device_base_id) != Load::Loaded || out.device_base_id.empty()) return false;
    const auto voucher = ReadString("voucher", out.voucher);
    if (voucher == Load::Unavailable) return false;
    if (!out.voucher.empty()) {
        CommissioningCredential parsed;
        if (!ParseCommissioningVoucher(out.voucher, parsed) || parsed.device_base_id != out.device_base_id) return false;
        out = std::move(parsed);
    }
    return true;
}

bool EspIdfCommissioningCredentialStore::Prepare(
    const std::string& owner, uint64_t owner_generation, uint32_t generation,
    bool replacement, PreparedCommissioningIdentity& out) {
    replacement = replacement || CommissioningTransactionNeedsFreshIdentity();
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (generation == 0 || owner.empty() || owner_generation == 0) return false;
    Identity pending;
    const auto loaded = ReadIdentity(kPending, pending);
    if (loaded == Load::Unavailable) return false;
    if (loaded == Load::Loaded && pending.generation == generation &&
        pending.credential.owner_domain_id == owner &&
        pending.credential.owner_domain_generation == owner_generation) {
        return DeviceIdentity::DescribeKey(pending.pem, out.fingerprint, out.device_instance_id);
    }
    Identity active;
    if (ReadIdentity(kActive, active) == Load::Unavailable) return false;
    CommissioningCredential previous;
    const bool has_credential = Load(previous);
    auto& device = DeviceIdentity::GetInstance();
    if (device.EnsureKeypair() != ESP_OK) return false;
    const std::string current_key = "sha256:" + device.DeviceInstanceId().substr(16);
    if (has_credential && !previous.owner_domain_id.empty()) {
        replacement = replacement || previous.owner_domain_id != owner ||
            previous.operational_key_id != current_key ||
            (previous.owner_domain_generation != 0 && previous.owner_domain_generation != owner_generation);
    }
    HubConfigStore claims;
    ActiveClaimState claim;
    EnrollmentJournalEntry enrollment;
    const auto claim_load = claims.LoadActiveClaim(claim);
    const auto enrollment_load = claims.LoadEnrollment(enrollment);
    if (claim_load == ClaimStoreLoadResult::StorageFailure ||
        enrollment_load == ClaimStoreLoadResult::StorageFailure) return false;
    if (claim_load == ClaimStoreLoadResult::Loaded) {
        replacement = replacement || claim.device_ref.owner_domain_id.value != owner ||
            claim.device_ref.owner_domain_generation != owner_generation ||
            claim.device_ref.device_instance_id != device.DeviceInstanceId();
    }
    if (enrollment_load == ClaimStoreLoadResult::Loaded) {
        replacement = replacement || enrollment.owner_domain_id.value != owner ||
            enrollment.owner_domain_generation != owner_generation ||
            enrollment.device_instance_candidate_id != device.DeviceInstanceId();
    }
    pending = {};
    pending.generation = generation;
    pending.replacement = replacement;
    if (!DeviceIdentity::PrepareKey(replacement, pending.pem, out.fingerprint, out.device_instance_id)) return false;
    if (!replacement && has_credential) pending.credential = previous;
    pending.credential.owner_domain_id = owner;
    pending.credential.owner_domain_generation = owner_generation;
    pending.credential.operational_key_id = out.fingerprint;
    // Only a current Claim or Enrollment can authorize voucher-free continuation.
    pending.ready = !replacement && has_credential &&
        ((claim_load == ClaimStoreLoadResult::Loaded && claim.state == ActiveClaimLocalState::Active) ||
         enrollment_load == ClaimStoreLoadResult::Loaded);
    return WriteString(kPending, Encode(pending));
}

bool EspIdfCommissioningCredentialStore::Stage(const CommissioningCredential* credential,
    uint32_t generation, const std::function<bool()>& guard) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    Identity pending;
    if (!guard || !guard() || ReadIdentity(kPending, pending) != Load::Loaded ||
        pending.generation != generation) return false;
    if (credential) {
        if (credential->owner_domain_id != pending.credential.owner_domain_id ||
            credential->owner_domain_generation != pending.credential.owner_domain_generation ||
            credential->operational_key_id != pending.credential.operational_key_id ||
            (!pending.replacement && !pending.credential.device_base_id.empty() &&
             pending.credential.device_base_id != credential->device_base_id)) return false;
        pending.credential = *credential;
        pending.ready = true;
    }
    if (!pending.ready || !guard()) return false;
    return WriteString(kPending, Encode(pending));
}

bool EspIdfCommissioningCredentialStore::DescribeCandidate(uint32_t generation,
    PreparedCommissioningIdentity& out) const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    Identity pending;
    return ReadIdentity(kPending, pending) == Load::Loaded && pending.generation == generation &&
        DeviceIdentity::DescribeKey(pending.pem, out.fingerprint, out.device_instance_id);
}

bool EspIdfCommissioningCredentialStore::StagedDigest(uint32_t generation,
    const std::string& owner, uint64_t owner_generation,
    std::string& digest, bool& replacement) const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    Identity pending;
    if (ReadIdentity(kPending, pending) != Load::Loaded || pending.generation != generation || !pending.ready ||
        pending.credential.owner_domain_id != owner || pending.credential.owner_domain_generation != owner_generation) return false;
    digest = Digest(Encode(pending));
    replacement = pending.replacement;
    return !digest.empty();
}

bool EspIdfCommissioningCredentialStore::RequiresRuntimeRestart(uint32_t generation) const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    Identity active;
    return ReadIdentity(kActive, active) == Load::Loaded && active.generation == generation && active.replacement;
}

bool EspIdfCommissioningCredentialStore::CommitStaged(uint32_t generation, const std::string& digest) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    Identity value;
    if (ReadIdentity(kActive, value) == Load::Loaded && value.generation == generation &&
        Digest(Encode(value)) == digest) return true;
    if (ReadIdentity(kPending, value) != Load::Loaded || !value.ready ||
        value.generation != generation || Digest(Encode(value)) != digest) return false;
    if (!WriteString(kActive, Encode(value))) return false;
    DeviceIdentity::GetInstance().ForgetCachedKeyAfterPhysicalRecovery();
    return true;
}

bool EspIdfCommissioningCredentialStore::Finish(uint32_t generation) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    Identity active;
    if (ReadIdentity(kActive, active) != Load::Loaded || active.generation != generation) return false;
    return Erase({"p256_priv", "base_id", "voucher", "voucher_jti", "voucher_exp", kPending});
}

bool EspIdfCommissioningCredentialStore::Rollback(uint32_t generation) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    Identity pending;
    const auto loaded = ReadIdentity(kPending, pending);
    if (loaded == Load::NotFound) return true;
    return loaded == Load::Loaded && pending.generation == generation && Erase({kPending});
}

bool EspIdfCommissioningCredentialStore::ForgetSpentVoucher() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    Identity active;
    const auto loaded = ReadIdentity(kActive, active);
    if (loaded == Load::Unavailable) return false;
    if (loaded == Load::NotFound) return Erase({"voucher", "voucher_jti", "voucher_exp"});
    if (active.credential.voucher.empty()) return true;
    active.credential.voucher.clear();
    active.credential.voucher_jti.clear();
    active.credential.voucher_expires_at_unix = 0;
    return WriteString(kActive, Encode(active));
}
}  // namespace eidolon
