#include "esp_idf_commissioning_credential_store.h"
#include "commissioning_transaction.h"

#include "device_identity.h"
#include "hub_config_store.h"
#include "mbedtls_sha256_compat.h"

#include <cJSON.h>
#include <esp_log.h>
#include <nvs.h>
#include <array>
#include <cstdlib>
#include <cerrno>
#include <mutex>

#define TAG "CommissioningCredentialStore"

namespace eidolon {
namespace {
std::recursive_mutex mutex;
constexpr const char* kActive = "id_active";
constexpr const char* kPending = "id_pending";
constexpr const char* kSelector = "id_slot";
constexpr const char* kStage = "id_stage";
const char* SlotKey(int slot) { return slot == 0 ? kActive : kPending; }
struct Candidate { uint32_t generation = 0; int slot = 0; bool reuse = false; };
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
    if (err != ESP_OK) ESP_LOGE(TAG, "Write identity key %s failed: 0x%x", key, err);
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

// The existing two records are slots, not source/destination of a copy. A
// missing selector means legacy slot 0, allowing an interrupted v1 journal to
// finish by activating its already durable id_pending without allocating it again.
bool ActiveSlot(int& slot) {
    nvs_handle_t handle = 0;
    auto err = nvs_open(kCommissioningCredentialNamespace, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) { slot = 0; return true; }
    if (err != ESP_OK) return false;
    uint8_t stored = 0;
    err = nvs_get_u8(handle, kSelector, &stored);
    nvs_close(handle);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return false;
    slot = stored;
    return slot <= 1;
}
Load ReadActive(Identity& value) {
    int slot;
    if (!ActiveSlot(slot)) return Load::Unavailable;
    const auto result = ReadIdentity(SlotKey(slot), value);
    // Once a selector exists, losing its record is corruption, not permission
    // to create a new key next to a retained base identity.
    if (result == Load::NotFound) {
        nvs_handle_t handle = 0;
        if (nvs_open(kCommissioningCredentialNamespace, NVS_READONLY, &handle) == ESP_OK) {
            uint8_t selected;
            const auto selected_result = nvs_get_u8(handle, kSelector, &selected);
            nvs_close(handle);
            if (selected_result == ESP_OK) return Load::Unavailable;
        }
    }
    return result;
}
Load ReadCandidate(uint32_t generation, Candidate& candidate, Identity& value) {
    std::string raw;
    const auto loaded = ReadString(kStage, raw);
    int active;
    if (!ActiveSlot(active)) return Load::Unavailable;
    if (loaded == Load::Unavailable) return loaded;
    if (loaded == Load::NotFound) {
        // An interrupted old-format transaction has no stage marker.
        candidate = {generation, 1 - active, false};
        const auto legacy = ReadIdentity(SlotKey(candidate.slot), value);
        return legacy == Load::Loaded && value.generation != generation
            ? Load::Unavailable : legacy;
    }
    cJSON* root = cJSON_Parse(raw.c_str());
    const auto* gen = cJSON_GetObjectItemCaseSensitive(root, "generation");
    const auto* slot = cJSON_GetObjectItemCaseSensitive(root, "slot");
    const auto* reuse = cJSON_GetObjectItemCaseSensitive(root, "reuse");
    const bool valid = cJSON_IsNumber(gen) && gen->valuedouble == generation &&
        cJSON_IsNumber(slot) && (slot->valuedouble == 0 || slot->valuedouble == 1) && cJSON_IsBool(reuse);
    if (valid) candidate = {generation, slot->valueint, cJSON_IsTrue(reuse) != 0};
    cJSON_Delete(root);
    if (!valid || (candidate.reuse && candidate.slot != active)) return Load::Unavailable;
    return ReadIdentity(SlotKey(candidate.slot), value);
}
bool BindCandidate(const Candidate& candidate) {
    return WriteString(kStage, "{\"generation\":" + std::to_string(candidate.generation) +
        ",\"slot\":" + std::to_string(candidate.slot) +
        ",\"reuse\":" + (candidate.reuse ? "true}" : "false}"));
}
bool SelectSlot(int slot) {
    nvs_handle_t handle = 0;
    if (nvs_open(kCommissioningCredentialNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    auto err = nvs_set_u8(handle, kSelector, static_cast<uint8_t>(slot));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
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
    const auto loaded = ReadActive(active);
    if (loaded == Load::Loaded) {
        std::string key_id, instance;
        const bool describable = DeviceIdentity::DescribeKey(active.pem, key_id, instance);
        if (!active.ready || !describable ||
            key_id != active.credential.operational_key_id) {
            // Three ways for a stored identity to be incoherent, and they all
            // used to leave by the same unlabelled door. Which one it is
            // decides whether someone is looking at an interrupted rotation,
            // an unreadable key, or a key that is not the one this credential
            // was issued for.
            ESP_LOGE(TAG,
                     "active identity is incoherent: ready=%d describable=%d key_id_matches=%d",
                     active.ready ? 1 : 0, describable ? 1 : 0,
                     (describable && key_id == active.credential.operational_key_id) ? 1 : 0);
            return Load::Unavailable;
        }
        pem = active.pem;
        return loaded;
    }
    if (loaded == Load::Unavailable) {
        ESP_LOGE(TAG, "the active identity record could not be read");
        return loaded;
    }
    const auto legacy = ReadString("p256_priv", pem);
    if (legacy == Load::Loaded) {
        std::string key_id, instance;
        if (!DeviceIdentity::DescribeKey(pem, key_id, instance)) {
            ESP_LOGE(TAG, "legacy operational private key could not be parsed; preserving identity");
            pem.clear();
            return Load::Unavailable;
        }
    }
    if (legacy == Load::NotFound) {
        std::string base;
        if (ReadString("base_id", base) != Load::NotFound) {
            // The half identity the design names (R23, §4.1.1 E9): a base id
            // the Owner Domain minted, with no private key left to speak for
            // it. Refusing is right. Refusing in silence is what left a person
            // pressing a button that answered nothing.
            ESP_LOGE(TAG,
                     "half identity: a base id is present and no operational private key remains");
            return Load::Unavailable;
        }
    }
    return legacy;
}

bool EspIdfCommissioningCredentialStore::Load(CommissioningCredential& out) const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    out = {};
    Identity active;
    const auto loaded = ReadActive(active);
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
    Candidate candidate;
    const auto loaded = ReadCandidate(generation, candidate, pending);
    // A candidate from another completed/cancelled session is disposable only
    // before a durable decision. The transaction gate enforces that boundary.
    if (loaded == Load::Loaded && pending.credential.owner_domain_id == owner &&
        pending.credential.owner_domain_generation == owner_generation &&
        (!replacement || (!candidate.reuse && pending.replacement))) {
        out.requires_voucher = !candidate.reuse;
        return DeviceIdentity::DescribeKey(pending.pem, out.fingerprint, out.device_instance_id);
    }
    Identity active;
    if (ReadActive(active) == Load::Unavailable) return false;
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
    int active_slot;
    if (!ActiveSlot(active_slot)) return false;
    const bool continuing = !replacement && has_credential && active.ready &&
        active.credential.owner_domain_id == owner &&
        active.credential.owner_domain_generation == owner_generation &&
        active.credential.operational_key_id == current_key &&
        ((claim_load == ClaimStoreLoadResult::Loaded && claim.state == ActiveClaimLocalState::Active) ||
         (claim_load == ClaimStoreLoadResult::NotFound && enrollment_load == ClaimStoreLoadResult::Loaded));
    if (continuing) {
        out.requires_voucher = false;
        return DeviceIdentity::DescribeKey(active.pem, out.fingerprint, out.device_instance_id) &&
            BindCandidate({generation, active_slot, true});
    }
    pending = {};
    pending.generation = generation;
    pending.replacement = replacement;
    out.requires_voucher = true;
    if (!DeviceIdentity::PrepareKey(replacement, pending.pem, out.fingerprint, out.device_instance_id)) return false;
    if (!replacement && has_credential) pending.credential = previous;
    pending.credential.owner_domain_id = owner;
    pending.credential.owner_domain_generation = owner_generation;
    pending.credential.operational_key_id = out.fingerprint;
    return WriteString(SlotKey(1 - active_slot), Encode(pending)) &&
        BindCandidate({generation, 1 - active_slot, false});
}

bool EspIdfCommissioningCredentialStore::Stage(const CommissioningCredential* credential,
    uint32_t generation, const std::function<bool()>& guard) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    Identity pending;
    Candidate candidate;
    if (!guard || !guard() || ReadCandidate(generation, candidate, pending) != Load::Loaded) return false;
    if (credential) {
        if (credential->owner_domain_id != pending.credential.owner_domain_id ||
            credential->owner_domain_generation != pending.credential.owner_domain_generation ||
            credential->operational_key_id != pending.credential.operational_key_id ||
            (!pending.replacement && !pending.credential.device_base_id.empty() &&
             pending.credential.device_base_id != credential->device_base_id)) return false;
        // Older Controllers still send a fresh voucher on network maintenance.
        // Validate its identity binding, but do not mutate a live identity for it.
        if (!candidate.reuse) {
            pending.credential = *credential;
            pending.ready = true;
        }
    }
    if (!pending.ready || !guard()) return false;
    return candidate.reuse || WriteString(SlotKey(candidate.slot), Encode(pending));
}

bool EspIdfCommissioningCredentialStore::DescribeCandidate(uint32_t generation,
    PreparedCommissioningIdentity& out) const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    Identity pending;
    Candidate candidate;
    if (ReadCandidate(generation, candidate, pending) != Load::Loaded) return false;
    out.requires_voucher = !candidate.reuse;
    return DeviceIdentity::DescribeKey(pending.pem, out.fingerprint, out.device_instance_id);
}

bool EspIdfCommissioningCredentialStore::StagedDigest(uint32_t generation,
    const std::string& owner, uint64_t owner_generation,
    std::string& digest, bool& replacement) const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    Identity pending;
    Candidate candidate;
    if (ReadCandidate(generation, candidate, pending) != Load::Loaded || !pending.ready ||
        pending.credential.owner_domain_id != owner || pending.credential.owner_domain_generation != owner_generation) return false;
    digest = Digest(Encode(pending));
    replacement = !candidate.reuse && pending.replacement;
    return !digest.empty();
}

bool EspIdfCommissioningCredentialStore::CommitStaged(uint32_t generation, const std::string& digest) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    Identity value;
    // Snapshot equality makes replay independent of the boot-local generation
    // stored by the session that originally created this identity.
    if (ReadActive(value) == Load::Loaded && Digest(Encode(value)) == digest) return true;
    Candidate candidate;
    if (ReadCandidate(generation, candidate, value) != Load::Loaded || !value.ready ||
        Digest(Encode(value)) != digest) return false;
    if (!SelectSlot(candidate.slot)) return false;
    DeviceIdentity::GetInstance().ForgetCachedKeyAfterPhysicalRecovery();
    return true;
}

bool EspIdfCommissioningCredentialStore::Finish(uint32_t, const std::string& digest) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    Identity active;
    int slot;
    if (!ActiveSlot(slot) || ReadActive(active) != Load::Loaded ||
        !active.ready || Digest(Encode(active)) != digest) return false;
    return Erase({"p256_priv", "base_id", "voucher", "voucher_jti", "voucher_exp",
                  SlotKey(1 - slot), kStage});
}

bool EspIdfCommissioningCredentialStore::Rollback(uint32_t generation) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    int slot;
    if (!ActiveSlot(slot)) return false;
    Candidate candidate;
    Identity pending;
    const auto loaded = ReadCandidate(generation, candidate, pending);
    if (loaded == Load::NotFound) return Erase({kStage});
    if (loaded != Load::Loaded) return false;
    return (candidate.slot == slot || Erase({SlotKey(candidate.slot)})) && Erase({kStage});
}

bool EspIdfCommissioningCredentialStore::ForgetSpentVoucher() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    Identity active;
    const auto loaded = ReadActive(active);
    if (loaded == Load::Unavailable) return false;
    if (loaded == Load::NotFound) return Erase({"voucher", "voucher_jti", "voucher_exp"});
    if (active.credential.voucher.empty()) return true;
    active.credential.voucher.clear();
    active.credential.voucher_jti.clear();
    active.credential.voucher_expires_at_unix = 0;
    int slot;
    return ActiveSlot(slot) && WriteString(SlotKey(slot), Encode(active));
}
}  // namespace eidolon
