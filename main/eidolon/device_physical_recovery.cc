#include "device_physical_recovery.h"

#include "claim_recovery_core.h"
#include "device_identity.h"
#include "device_physical_recovery_core.h"
#include "hub_config_store.h"
#include "esp_idf_owner_data_erase_storage.h"

#include <cJSON.h>
#include <esp_log.h>
#include <nvs.h>

#include <array>
#include <initializer_list>

#define TAG "PhysicalRecovery"

namespace eidolon {
namespace {

constexpr char kRecoveryKey[] = "physical_rejoin";
constexpr int kRecoverySchema = 1;

std::string ReadString(nvs_handle_t handle, const char* key) {
    size_t size = 0;
    if (nvs_get_str(handle, key, nullptr, &size) != ESP_OK || size == 0) return {};
    std::string value(size, '\0');
    if (nvs_get_str(handle, key, value.data(), &size) != ESP_OK) return {};
    while (!value.empty() && value.back() == '\0') value.pop_back();
    return value;
}

class RecoveryJournal final : public PhysicalRecoveryJournalPort {
public:
    PhysicalRecoveryLoadResult Load(PhysicalRecoveryRecord& out) override {
        nvs_handle_t handle = 0;
        const esp_err_t opened = nvs_open(
            kDeviceEraseJournalNvsNamespace, NVS_READONLY, &handle);
        if (opened == ESP_ERR_NVS_NOT_FOUND) {
            return PhysicalRecoveryLoadResult::NotFound;
        }
        if (opened != ESP_OK) return PhysicalRecoveryLoadResult::StorageFailure;
        const std::string encoded = ReadString(handle, kRecoveryKey);
        nvs_close(handle);
        if (encoded.empty()) return PhysicalRecoveryLoadResult::NotFound;
        cJSON* root = cJSON_Parse(encoded.c_str());
        if (!root) return PhysicalRecoveryLoadResult::StorageFailure;
        const auto text = [root](const char* key) {
            cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
            return cJSON_IsString(item) && item->valuestring
                       ? std::string(item->valuestring)
                       : std::string{};
        };
        cJSON* schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
        cJSON* phase = cJSON_GetObjectItemCaseSensitive(root, "phase");
        out.operation_id = text("operation_id");
        out.old_device_instance_id = text("old_device_instance_id");
        out.terminal_signature = text("terminal_signature");
        out.new_device_instance_id = text("new_device_instance_id");
        const int phase_value = cJSON_IsNumber(phase) ? phase->valueint : 0;
        const bool valid = cJSON_IsNumber(schema) &&
            schema->valueint == kRecoverySchema &&
            phase_value >= static_cast<int>(PhysicalRecoveryPhase::Authorized) &&
            phase_value <= static_cast<int>(PhysicalRecoveryPhase::Commissionable) &&
            !out.operation_id.empty() && !out.old_device_instance_id.empty() &&
            !out.terminal_signature.empty() &&
            (phase_value < static_cast<int>(PhysicalRecoveryPhase::NewPrincipalCreated) ||
             !out.new_device_instance_id.empty());
        cJSON_Delete(root);
        if (!valid) return PhysicalRecoveryLoadResult::StorageFailure;
        out.phase = static_cast<PhysicalRecoveryPhase>(phase_value);
        return PhysicalRecoveryLoadResult::Loaded;
    }

    bool Store(const PhysicalRecoveryRecord& value) override {
        cJSON* root = cJSON_CreateObject();
        if (!root) return false;
        cJSON_AddNumberToObject(root, "schema", kRecoverySchema);
        cJSON_AddNumberToObject(root, "phase", static_cast<int>(value.phase));
        cJSON_AddStringToObject(root, "operation_id", value.operation_id.c_str());
        cJSON_AddStringToObject(root, "old_device_instance_id",
                                value.old_device_instance_id.c_str());
        cJSON_AddStringToObject(root, "terminal_signature",
                                value.terminal_signature.c_str());
        cJSON_AddStringToObject(root, "new_device_instance_id",
                                value.new_device_instance_id.c_str());
        char* encoded = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!encoded) return false;
        nvs_handle_t handle = 0;
        esp_err_t result = nvs_open(
            kDeviceEraseJournalNvsNamespace, NVS_READWRITE, &handle);
        if (result == ESP_OK) result = nvs_set_str(handle, kRecoveryKey, encoded);
        if (result == ESP_OK) result = nvs_commit(handle);
        cJSON_free(encoded);
        if (handle != 0) nvs_close(handle);
        return result == ESP_OK;
    }
};

bool EraseKeys(const char* name_space,
               const std::initializer_list<const char*>& keys) {
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(name_space, NVS_READWRITE, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;
    if (result != ESP_OK) return false;
    for (const char* key : keys) {
        result = nvs_erase_key(handle, key);
        if (result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;
        if (result != ESP_OK) break;
    }
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}

class RecoveryIdentity final : public PhysicalRecoveryIdentityPort {
public:
    bool ClearOldOwnerAndOperationalState() override {
        // This is an explicit allowlist. Factory identity, RF/audio/display
        // calibration and the protected RemovalJournal are never namespaces or
        // keys in this list.
        const bool cleared =
            EraseKeys("eidolon", {"config", "enrollment", "active_claim"}) &&
            EraseKeys("eidolon_claim", {"handoff_priv"}) &&
            EraseKeys("eidolon_id", {"p256_priv"});
        if (cleared) {
            DeviceIdentity::GetInstance().ForgetCachedKeyAfterPhysicalRecovery();
        }
        return cleared;
    }

    bool CreateFreshOperationalIdentity(std::string& instance_id) override {
        auto& identity = DeviceIdentity::GetInstance();
        if (identity.EnsureKeypair() != ESP_OK) return false;
        instance_id = identity.DeviceInstanceId();
        return !instance_id.empty();
    }
};

PhysicalRecoveryResult Run(const DeviceEraseJournalEntry& terminal,
                           bool physical_presence) {
    RecoveryJournal journal;
    RecoveryIdentity identity;
    DevicePhysicalRecoveryCore core(journal, identity);
    const auto result = physical_presence
        ? core.AuthorizeAndRun(terminal, true)
        : core.Resume(terminal);
    if (result != PhysicalRecoveryResult::Commissionable ||
        terminal.phase == DeviceEraseJournalPhase::ArchivedTerminal) {
        return result;
    }
    DeviceEraseJournalEntry archived = terminal;
    archived.phase = DeviceEraseJournalPhase::ArchivedTerminal;
    EspIdfDeviceEraseJournal removal;
    return removal.Store(archived) ? result
                                   : PhysicalRecoveryResult::StorageFailure;
}

}  // namespace

bool DevicePhysicalRecovery::AuthorizeFromPhysicalPresence() {
    EspIdfDeviceEraseJournal removal;
    DeviceEraseJournalEntry terminal;
    const auto loaded = removal.Load(terminal);
    if (loaded == DeviceEraseJournalLoadResult::NotFound) {
        return ClearRevokedClaimOnPhysicalPresence();
    }
    if (loaded != DeviceEraseJournalLoadResult::Loaded) return false;
    return Run(terminal, true) == PhysicalRecoveryResult::Commissionable;
}

bool DevicePhysicalRecovery::ClearRevokedClaimOnPhysicalPresence() {
    // A Claim the Owner revoked is terminal on purpose: the device may not
    // resurrect it by asking again on its own. But it left the device with a
    // Claim nothing could use and no way to give it up, so the only remaining
    // way back was erasing NVS — which mints a new identity, and so returns a
    // different device than the one the Owner removed.
    //
    // Physical presence is the authority this design already uses for handing a
    // device to a Host, so it is the authority for giving up a Claim too. The
    // Claim is all that is dropped: the identity stays, so the device that
    // proposes again is the same device, and the Owner approves it as they did
    // the first time.
    HubConfigStore store;
    ActiveClaimState claim;
    const ClaimStoreLoadResult loaded = store.LoadActiveClaim(claim);
    if (loaded == ClaimStoreLoadResult::StorageFailure) return false;
    if (loaded != ClaimStoreLoadResult::Loaded) return true;
    if (!PhysicalPresenceMayClearStoredClaim(claim)) return true;
    if (!store.ClearActiveClaim() || !store.ClearEnrollment()) return false;
    ESP_LOGW(TAG, "Physical presence gave up a revoked Claim; this device can be "
                  "claimed again as %s",
             claim.device_ref.device_instance_id.c_str());
    return true;
}

bool DevicePhysicalRecovery::ResumeAuthorizedTerminal(
    const DeviceEraseJournalEntry& terminal) {
    return Run(terminal, false) == PhysicalRecoveryResult::Commissionable;
}

}  // namespace eidolon
