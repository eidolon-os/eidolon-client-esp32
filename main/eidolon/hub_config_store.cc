#include "hub_config_store.h"

#include "device_foundation_v1_generated.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_log.h>

#include <limits>

#define TAG "HubConfigStore"

namespace eidolon {

// The whole Hub config is persisted as a single JSON object under one NVS key.
// This makes a save atomic: a crash/power-loss either leaves the previous value
// intact or commits the new one whole — never a torn mix of old and new fields
// (e.g. a fresh server_url paired with a stale token).
static constexpr const char* kConfigKey = "config";
static constexpr const char* kEnrollmentJournalKey = "enrollment";
static constexpr const char* kActiveClaimKey = "active_claim";
// Version 4 holds one channel. Version 3 held a voice room and a control room,
// and a device reading those back would connect to a pair that no longer exists
// — the Provider issues one. Refusing the old record sends the device to
// re-provision, which is the only way to be handed the new one.
static constexpr int kConfigSchemaVersion = 5;
// Version 1 is the canonical screen-independent Enrollment journal. Older
// onboarding records used a different key and are intentionally not read.
static constexpr int kEnrollmentJournalSchemaVersion = 1;
static constexpr int kActiveClaimSchemaVersion = 2;

static std::string JsonStringField(cJSON* root, const char* key) {
    cJSON* item = cJSON_GetObjectItem(root, key);
    return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : "";
}

static int JsonIntField(cJSON* root, const char* key, int fallback) {
    cJSON* item = cJSON_GetObjectItem(root, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static bool JsonUintField(cJSON* root, const char* key, uint64_t& out) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < 1 ||
        item->valuedouble > 9007199254740991.0) {
        return false;
    }
    const uint64_t value = static_cast<uint64_t>(item->valuedouble);
    if (static_cast<double>(value) != item->valuedouble) return false;
    out = value;
    return true;
}

static bool Digest(const std::string& value) {
    return value.size() == 71 && value.rfind("sha256:", 0) == 0 &&
           value.find_first_not_of("0123456789abcdef", 7) == std::string::npos;
}

static bool AsciiAlphaNumeric(unsigned char value) {
    return (value >= '0' && value <= '9') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z');
}

// A device instance id is a digest of this device's operational key, so it is
// checked as one rather than as "any identifier". The looser check accepted
// whatever a journal happened to hold, including the MAC-shaped ids the
// contract's own vectors used to carry.
static bool IsDeviceInstanceId(const std::string& value) {
    return device_foundation::v1::DeviceInstanceId::Parse(value).has_value();
}

static bool Identifier(const std::string& value) {
    if (value.size() < 3 || value.size() > 128 ||
        !AsciiAlphaNumeric(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    for (const unsigned char character : value) {
        if (!AsciiAlphaNumeric(character) && character != '.' &&
            character != '_' && character != ':' && character != '-') {
            return false;
        }
    }
    return true;
}

static bool OwnerDomainId(const std::string& value) {
    return value.rfind("owner-", 0) == 0 && value.size() > 6 &&
           AsciiAlphaNumeric(static_cast<unsigned char>(value[6])) &&
           Identifier(value);
}

static bool Base64Url(const std::string& value, size_t minimum,
                      size_t maximum) {
    if (value.size() < minimum || value.size() > maximum) return false;
    for (const unsigned char character : value) {
        if (!AsciiAlphaNumeric(character) && character != '-' &&
            character != '_') {
            return false;
        }
    }
    return true;
}

static bool JsonSafeUint(uint64_t value) {
    return value > 0 && value <= 9007199254740991ULL;
}

esp_err_t HubConfigStore::SaveHubConfig(const Esp32HubConfig& config) {
    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(root, "schema_version", kConfigSchemaVersion);
    cJSON_AddStringToObject(root, "status", HubConfigStatusToString(config.status));
    cJSON_AddStringToObject(root, "server_url", config.session.server_url.c_str());
    cJSON_AddStringToObject(root, "token", config.session.token.c_str());
    cJSON_AddStringToObject(root, "identity", config.session.identity.c_str());
    cJSON_AddStringToObject(root, "room_name", config.session.room_name.c_str());
    cJSON_AddNumberToObject(root, "expires_at_ms",
                           static_cast<double>(config.expires_at_ms));
    cJSON_AddNumberToObject(root, "sample_rate", config.sample_rate);
    cJSON_AddNumberToObject(root, "channels", config.channels);

    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        return ESP_ERR_NO_MEM;
    }
    Settings settings(kNvsNamespace, true);
    const esp_err_t write_err = settings.SetString(kConfigKey, printed);
    cJSON_free(printed);
    if (write_err != ESP_OK) {
        return write_err;
    }
    const esp_err_t commit_err = settings.Commit();
    if (commit_err != ESP_OK) {
        return commit_err;
    }

    ESP_LOGI(TAG, "Saved Hub config identity=%s status=%s room=%s server=%s",
             config.session.identity.c_str(), HubConfigStatusToString(config.status),
             config.session.room_name.c_str(), config.session.server_url.c_str());
    return ESP_OK;
}

bool HubConfigStore::HasValidConfig() const {
    Esp32HubConfig config;
    return Load(config);
}

bool HubConfigStore::Load(Esp32HubConfig& config) const {
    Settings settings(kNvsNamespace, false);
    std::string blob = settings.GetString(kConfigKey);
    if (blob.empty()) {
        return false;
    }
    cJSON* root = cJSON_Parse(blob.c_str());
    if (!root) {
        ESP_LOGW(TAG, "Stored Hub config is not valid JSON; treating as no config");
        return false;
    }

    if (JsonIntField(root, "schema_version", 0) != kConfigSchemaVersion) {
        cJSON_Delete(root);
        return false;
    }

    config = Esp32HubConfig{};
    // A missing/unknown status parses to the most conservative state
    // (PendingApproval) — never silently grant voice on an absent field.
    config.status = ParseHubConfigStatus(JsonStringField(root, "status"));
    config.session.server_url = JsonStringField(root, "server_url");
    config.session.token = JsonStringField(root, "token");
    config.session.identity = JsonStringField(root, "identity");
    config.session.room_name = JsonStringField(root, "room_name");
    const cJSON* expires = cJSON_GetObjectItem(root, "expires_at_ms");
    if (cJSON_IsNumber(expires) && expires->valuedouble >= 0) {
        config.expires_at_ms = static_cast<int64_t>(expires->valuedouble);
    }
    config.sample_rate = JsonIntField(root, "sample_rate", 16000);
    config.channels = JsonIntField(root, "channels", 1);

    const bool valid =
        config.status != HubConfigStatus::Active || config.session.usable();
    cJSON_Delete(root);
    return valid;
}

bool HubConfigStore::StoreEnrollment(const EnrollmentJournalEntry& state) {
    const bool proposal_created =
        state.phase == EnrollmentJournalPhase::ProposalCreated;
    const bool grant_staged =
        state.phase == EnrollmentJournalPhase::GrantStaged;
    const bool empty_stage = state.grant_id.empty() &&
        state.approval_decision_id.empty() &&
        state.staged_device_ref.device_instance_id.empty() &&
        state.staged_device_ref.owner_domain_id.value.empty() &&
        state.staged_device_ref.owner_domain_generation == 0 &&
        state.staged_device_ref.claim_generation == 0 &&
        state.staged_device_ref.trust_epoch == 0;
    const bool valid_stage = Identifier(state.grant_id) &&
        Identifier(state.approval_decision_id) &&
        state.staged_device_ref.device_instance_id ==
            state.device_instance_candidate_id &&
        state.staged_device_ref.owner_domain_id.value ==
            state.owner_domain_id.value &&
        state.staged_device_ref.owner_domain_generation ==
            state.owner_domain_generation &&
        state.staged_device_ref.claim_generation > 0 &&
        state.staged_device_ref.trust_epoch > 0;
    if ((!proposal_created && !grant_staged) ||
        (proposal_created && !empty_stage) ||
        (grant_staged && !valid_stage) ||
        !OwnerDomainId(state.owner_domain_id.value) ||
        !JsonSafeUint(state.owner_domain_generation) ||
        !IsDeviceInstanceId(state.device_instance_candidate_id) ||
        !Identifier(state.enrollment_id) ||
        !JsonSafeUint(state.proposal_revision) ||
        !Base64Url(state.collection_challenge, 22, 128) ||
        !Digest(state.hardware_evidence_digest) ||
        !Identifier(state.manifest_ref.manifest_id) ||
        !JsonSafeUint(state.manifest_ref.revision) ||
        !Digest(state.manifest_ref.digest) ||
        !Digest(state.handoff_key_id) || !Digest(state.operational_key_id)) {
        return false;
    }
    cJSON* root = cJSON_CreateObject();
    if (!root) return false;
    cJSON_AddNumberToObject(root, "schema_version",
                           kEnrollmentJournalSchemaVersion);
    cJSON_AddNumberToObject(root, "phase", static_cast<int>(state.phase));
    cJSON_AddStringToObject(root, "owner_domain_id",
                            state.owner_domain_id.value.c_str());
    cJSON_AddNumberToObject(root, "owner_domain_generation",
                           static_cast<double>(state.owner_domain_generation));
    cJSON_AddStringToObject(root, "device_instance_candidate_id",
                            state.device_instance_candidate_id.c_str());
    cJSON_AddStringToObject(root, "enrollment_id", state.enrollment_id.c_str());
    cJSON_AddNumberToObject(root, "proposal_revision",
                            static_cast<double>(state.proposal_revision));
    cJSON_AddStringToObject(root, "collection_challenge",
                            state.collection_challenge.c_str());
    cJSON_AddStringToObject(root, "hardware_evidence_digest",
                            state.hardware_evidence_digest.c_str());
    cJSON_AddStringToObject(root, "manifest_id",
                            state.manifest_ref.manifest_id.c_str());
    cJSON_AddNumberToObject(root, "manifest_revision",
                            static_cast<double>(state.manifest_ref.revision));
    cJSON_AddStringToObject(root, "manifest_digest",
                            state.manifest_ref.digest.c_str());
    cJSON_AddStringToObject(root, "handoff_key_id",
                            state.handoff_key_id.c_str());
    cJSON_AddStringToObject(root, "operational_key_id",
                            state.operational_key_id.c_str());
    cJSON_AddStringToObject(root, "grant_id", state.grant_id.c_str());
    cJSON_AddStringToObject(root, "approval_decision_id",
                            state.approval_decision_id.c_str());
    cJSON_AddStringToObject(root, "staged_device_instance_id",
                            state.staged_device_ref.device_instance_id.c_str());
    cJSON_AddStringToObject(root, "staged_owner_domain_id",
                            state.staged_device_ref.owner_domain_id.value.c_str());
    cJSON_AddNumberToObject(root, "staged_owner_domain_generation",
                            static_cast<double>(
                                state.staged_device_ref.owner_domain_generation));
    cJSON_AddNumberToObject(root, "staged_claim_generation",
                            state.staged_device_ref.claim_generation);
    cJSON_AddNumberToObject(root, "staged_trust_epoch",
                            state.staged_device_ref.trust_epoch);
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) return false;
    Settings settings(kNvsNamespace, true);
    const esp_err_t write_err =
        settings.SetString(kEnrollmentJournalKey, printed);
    cJSON_free(printed);
    return write_err == ESP_OK && settings.Commit() == ESP_OK;
}

ClaimStoreLoadResult HubConfigStore::LoadEnrollment(
    EnrollmentJournalEntry& state) {
    Settings settings(kNvsNamespace, false);
    const std::string blob = settings.GetString(kEnrollmentJournalKey);
    if (blob.empty()) return ClaimStoreLoadResult::NotFound;
    cJSON* root = blob.empty() ? nullptr : cJSON_Parse(blob.c_str());
    if (!root || JsonIntField(root, "schema_version", 0) !=
                     kEnrollmentJournalSchemaVersion) {
        cJSON_Delete(root);
        return ClaimStoreLoadResult::StorageFailure;
    }
    state = {};
    const int phase = JsonIntField(root, "phase", 0);
    state.phase = static_cast<EnrollmentJournalPhase>(phase);
    state.owner_domain_id.value = JsonStringField(root, "owner_domain_id");
    uint64_t staged_claim_generation = 0;
    uint64_t staged_trust_epoch = 0;
    const bool numeric =
        JsonUintField(root, "owner_domain_generation",
                      state.owner_domain_generation) &&
        JsonUintField(root, "proposal_revision", state.proposal_revision) &&
        JsonUintField(root, "manifest_revision",
                      state.manifest_ref.revision);
    state.device_instance_candidate_id =
        JsonStringField(root, "device_instance_candidate_id");
    state.enrollment_id = JsonStringField(root, "enrollment_id");
    state.collection_challenge =
        JsonStringField(root, "collection_challenge");
    state.hardware_evidence_digest =
        JsonStringField(root, "hardware_evidence_digest");
    state.manifest_ref.manifest_id = JsonStringField(root, "manifest_id");
    state.manifest_ref.digest = JsonStringField(root, "manifest_digest");
    state.handoff_key_id = JsonStringField(root, "handoff_key_id");
    state.operational_key_id = JsonStringField(root, "operational_key_id");
    state.grant_id = JsonStringField(root, "grant_id");
    state.approval_decision_id =
        JsonStringField(root, "approval_decision_id");
    state.staged_device_ref.device_instance_id =
        JsonStringField(root, "staged_device_instance_id");
    state.staged_device_ref.owner_domain_id.value =
        JsonStringField(root, "staged_owner_domain_id");
    JsonUintField(root, "staged_owner_domain_generation",
                  state.staged_device_ref.owner_domain_generation);
    JsonUintField(root, "staged_claim_generation", staged_claim_generation);
    JsonUintField(root, "staged_trust_epoch", staged_trust_epoch);
    const bool generated_widths =
        staged_claim_generation <= std::numeric_limits<uint32_t>::max() &&
        staged_trust_epoch <= std::numeric_limits<uint32_t>::max();
    if (generated_widths) {
        state.staged_device_ref.claim_generation =
            static_cast<uint32_t>(staged_claim_generation);
        state.staged_device_ref.trust_epoch =
            static_cast<uint32_t>(staged_trust_epoch);
    }
    cJSON_Delete(root);
    const bool base = numeric && generated_widths &&
        (phase == static_cast<int>(EnrollmentJournalPhase::ProposalCreated) ||
         phase == static_cast<int>(EnrollmentJournalPhase::GrantStaged)) &&
        OwnerDomainId(state.owner_domain_id.value) &&
        IsDeviceInstanceId(state.device_instance_candidate_id) &&
        Identifier(state.enrollment_id) &&
        Base64Url(state.collection_challenge, 22, 128) &&
        Digest(state.hardware_evidence_digest) &&
        Identifier(state.manifest_ref.manifest_id) &&
        Digest(state.manifest_ref.digest) && Digest(state.handoff_key_id) &&
        Digest(state.operational_key_id);
    const bool staged =
        phase == static_cast<int>(EnrollmentJournalPhase::ProposalCreated)
        ? state.grant_id.empty() && state.approval_decision_id.empty() &&
            state.staged_device_ref.device_instance_id.empty() &&
            state.staged_device_ref.owner_domain_id.value.empty() &&
            state.staged_device_ref.owner_domain_generation == 0 &&
            state.staged_device_ref.claim_generation == 0 &&
            state.staged_device_ref.trust_epoch == 0
        : Identifier(state.grant_id) &&
            Identifier(state.approval_decision_id) &&
            state.staged_device_ref.device_instance_id ==
                state.device_instance_candidate_id &&
            state.staged_device_ref.owner_domain_id.value ==
                state.owner_domain_id.value &&
            state.staged_device_ref.owner_domain_generation ==
                state.owner_domain_generation &&
            state.staged_device_ref.claim_generation > 0 &&
            state.staged_device_ref.trust_epoch > 0;
    return base && staged ? ClaimStoreLoadResult::Loaded
                          : ClaimStoreLoadResult::StorageFailure;
}

bool HubConfigStore::ClearEnrollment() {
    Settings settings(kNvsNamespace, true);
    // Settings treats an absent key as an idempotent success and fail-stops on
    // any other NVS erase error.
    settings.EraseKey(kEnrollmentJournalKey);
    return settings.Commit() == ESP_OK;
}

bool HubConfigStore::ClearActiveClaim() {
    Settings settings(kNvsNamespace, true);
    // Settings treats an absent key as an idempotent success and fail-stops on
    // any other NVS erase error.
    settings.EraseKey(kActiveClaimKey);
    return settings.Commit() == ESP_OK;
}

bool HubConfigStore::StoreActiveClaim(const ActiveClaimState& state) {
    if (!state.valid() ||
        !JsonSafeUint(state.device_ref.owner_domain_generation) ||
        !JsonSafeUint(state.manifest_ref.revision)) {
        return false;
    }
    const auto& ref = state.device_ref;
    cJSON* root = cJSON_CreateObject();
    if (!root) return false;
    cJSON_AddNumberToObject(root, "schema_version", kActiveClaimSchemaVersion);
    cJSON_AddStringToObject(
        root, "claim_state",
        state.state == ActiveClaimLocalState::Active ? "active" : "revoked");
    cJSON_AddStringToObject(root, "device_instance_id", ref.device_instance_id.c_str());
    cJSON_AddStringToObject(root, "owner_domain_id",
                            ref.owner_domain_id.value.c_str());
    cJSON_AddNumberToObject(root, "owner_domain_generation",
                           static_cast<double>(ref.owner_domain_generation));
    cJSON_AddNumberToObject(root, "claim_generation", ref.claim_generation);
    cJSON_AddNumberToObject(root, "trust_epoch", ref.trust_epoch);
    cJSON_AddStringToObject(root, "manifest_id",
                            state.manifest_ref.manifest_id.c_str());
    cJSON_AddNumberToObject(root, "manifest_revision",
                            static_cast<double>(state.manifest_ref.revision));
    cJSON_AddStringToObject(root, "manifest_digest",
                            state.manifest_ref.digest.c_str());
    cJSON_AddStringToObject(root, "grant_id", state.grant_id.c_str());
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) return false;
    Settings settings(kNvsNamespace, true);
    const esp_err_t write_err = settings.SetString(kActiveClaimKey, printed);
    cJSON_free(printed);
    return write_err == ESP_OK && settings.Commit() == ESP_OK;
}

ClaimStoreLoadResult HubConfigStore::LoadActiveClaim(ActiveClaimState& state) {
    Settings settings(kNvsNamespace, false);
    const std::string blob = settings.GetString(kActiveClaimKey);
    if (blob.empty()) return ClaimStoreLoadResult::NotFound;
    cJSON* root = blob.empty() ? nullptr : cJSON_Parse(blob.c_str());
    if (!root ||
        JsonIntField(root, "schema_version", 0) != kActiveClaimSchemaVersion) {
        cJSON_Delete(root);
        return ClaimStoreLoadResult::StorageFailure;
    }
    state = {};
    const std::string claim_state = JsonStringField(root, "claim_state");
    state.state = claim_state == "active" ? ActiveClaimLocalState::Active
                                           : ActiveClaimLocalState::Revoked;
    auto& ref = state.device_ref;
    ref.device_instance_id = JsonStringField(root, "device_instance_id");
    ref.owner_domain_id.value = JsonStringField(root, "owner_domain_id");
    uint64_t claim_generation = 0;
    uint64_t trust_epoch = 0;
    const bool numeric =
        JsonUintField(root, "owner_domain_generation",
                      ref.owner_domain_generation) &&
        JsonUintField(root, "claim_generation", claim_generation) &&
        JsonUintField(root, "trust_epoch", trust_epoch) &&
        JsonUintField(root, "manifest_revision", state.manifest_ref.revision);
    const bool generated_widths =
        claim_generation <= std::numeric_limits<uint32_t>::max() &&
        trust_epoch <= std::numeric_limits<uint32_t>::max();
    if (generated_widths) {
        ref.claim_generation = static_cast<uint32_t>(claim_generation);
        ref.trust_epoch = static_cast<uint32_t>(trust_epoch);
    }
    state.manifest_ref.manifest_id = JsonStringField(root, "manifest_id");
    state.manifest_ref.digest = JsonStringField(root, "manifest_digest");
    state.grant_id = JsonStringField(root, "grant_id");
    cJSON_Delete(root);
    return numeric && generated_widths &&
                   (claim_state == "active" || claim_state == "revoked") &&
                   state.valid()
               ? ClaimStoreLoadResult::Loaded
               : ClaimStoreLoadResult::StorageFailure;
}

}  // namespace eidolon
