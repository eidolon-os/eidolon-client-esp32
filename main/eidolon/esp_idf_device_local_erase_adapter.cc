#include "esp_idf_device_local_erase_adapter.h"

#include <algorithm>
#include <set>

namespace eidolon {
namespace {

constexpr const char* kOwnerCredentials = "owner-credentials";
constexpr const char* kOwnerData = "owner-data";
constexpr const char* kNetworkProfiles = "network-profiles";

OwnerDataEraseTarget NvsKeys(
    const char* id, const char* scope, const char* partition,
    const char* nvs_namespace, std::vector<std::string> keys,
    bool optional = false, bool finalization_only = false) {
    return OwnerDataEraseTarget{
        id, scope, OwnerDataEraseTargetKind::NvsKeys, partition,
        nvs_namespace, std::move(keys), optional, finalization_only};
}

OwnerDataEraseTarget Partition(
    const char* id, const char* scope, const char* partition,
    bool optional = false) {
    return OwnerDataEraseTarget{
        id, scope, OwnerDataEraseTargetKind::DataPartition, partition,
        "", {}, optional, false};
}

bool Success(OwnerDataEraseStorageResult result) {
    return result == OwnerDataEraseStorageResult::Done ||
           result == OwnerDataEraseStorageResult::NotFound;
}

}  // namespace

const std::vector<std::string>&
EspIdfDeviceLocalEraseAdapter::ProtectedNvsNamespaces() {
    static const std::vector<std::string> protected_namespaces = {
        "board",          // persistent hardware UUID / asset identity
        "audio",          // codec calibration and gain/volume baseline
        "display",        // panel/backlight calibration and safe defaults
        "assets",         // installed asset revision/download metadata
        "network",        // board transport selection, not a Wi-Fi profile
        "vendor",         // physical control/product configuration
        "aec",            // acoustic calibration
        "otto_trims",     // actuator calibration
        "electron_trims", // actuator calibration
        "dog_trims",      // actuator calibration
        "edarobot_trims", // actuator calibration
        "led_strip",      // board LED calibration/defaults
        "model",          // board model selection
        "sparkbot",       // board orientation/calibration
        "lcd_display",    // panel calibration
        "eid_erase",      // RemovalJournal and signed terminal evidence
    };
    return protected_namespaces;
}

const std::vector<std::string>&
EspIdfDeviceLocalEraseAdapter::ProtectedPartitions() {
    static const std::vector<std::string> protected_partitions = {
        "nvsfactory",     // factory-provisioned identity/configuration
        "nvs_keys",       // NVS encryption keys
        "phy_init",       // radio calibration
        "assets",         // product UI/audio assets
        "assets_A",       // legacy product asset bank
        "human_face_det", // factory model asset
        "human_face_feat",// factory model asset
        "model",          // board-selected product model asset
        "otadata",        // boot/OTA selection state
        "ota_0", "ota_1", "factory", // executable images
    };
    return protected_partitions;
}

OwnerDataErasePlan EspIdfDeviceLocalEraseAdapter::BuildErasePlan(
    const device_foundation::v1::DeviceLocalEraseCommand& command) {
    OwnerDataErasePlan plan;
    std::set<std::string> requested;
    for (const auto& scope : command.erase_scopes) {
        if ((scope != kOwnerCredentials && scope != kOwnerData &&
             scope != kNetworkProfiles) ||
            !requested.insert(scope).second) {
            plan.refusal_code = "PROTECTED_FACTORY_SCOPE_REFUSED";
            return plan;
        }
    }
    if (requested.empty()) {
        plan.refusal_code = "PROTECTED_FACTORY_SCOPE_REFUSED";
        return plan;
    }

    const bool credentials = requested.count(kOwnerCredentials) != 0;
    const bool owner_data = requested.count(kOwnerData) != 0;
    const bool network = requested.count(kNetworkProfiles) != 0;
    plan.plan_id = std::string(credentials ? "C" : "-") +
                   (owner_data ? "D" : "-") +
                   (network ? "N" : "-");

    if (credentials) {
        const std::vector<std::string> trust_keys = {
            "trust_active", "trust_stage", "trust_sgen",
            "ot0_owner", "ot0_root", "ot0_auth", "ot0_desc", "ot0_digest",
            "ot1_owner", "ot1_root", "ot1_auth", "ot1_desc", "ot1_digest",
            "owner_domain", "owner_root", "authority_cert", "owner_desc",
        };
        plan.targets.push_back(NvsKeys(
            "owner-trust-dedicated", kOwnerCredentials, "owner_trust",
            "eidolon", trust_keys, true));
        plan.targets.push_back(NvsKeys(
            "owner-trust-default-fallback", kOwnerCredentials, "",
            "eidolon", trust_keys, true));
        plan.targets.push_back(NvsKeys(
            "delivery-credential", kOwnerCredentials, "", "eidolon",
            {"config"}, true));
        plan.targets.push_back(NvsKeys(
            "legacy-mqtt-credential", kOwnerCredentials, "", "mqtt",
            {"endpoint", "client_id", "username", "password", "keepalive",
             "publish_topic"}, true));
        plan.targets.push_back(NvsKeys(
            "legacy-websocket-credential", kOwnerCredentials, "", "websocket",
            {"url", "token", "version"}, true));
    }
    if (owner_data) {
        plan.targets.push_back(NvsKeys(
            "active-claim-and-onboarding", kOwnerData, "", "eidolon",
            {"onboarding", "active_claim", "ctx_gen", "ctx_phase",
             "ctx_owner", "ctx_ssid", "ctx_digest"}, true));
        plan.targets.push_back(NvsKeys(
            "device-user-preferences", kOwnerData, "", "eidolon_device",
            {"mic_enabled", "theme_applied"}, true));
        plan.targets.push_back(NvsKeys(
            "owner-face-metadata", kOwnerData, "", "owner_face",
            {"schema", "state", "slot", "revision", "templates", "binding",
             "profile", "model", "preproc"}, true));
        plan.targets.push_back(Partition(
            "owner-face-db", kOwnerData, "face_db", true));
    }
    if (network) {
        std::vector<std::string> wifi_keys = {
            "ota_url", "max_tx_power", "remember_bssid", "sleep_mode"};
        for (int index = 0; index < 10; ++index) {
            wifi_keys.push_back(index == 0 ? "ssid" :
                                "ssid" + std::to_string(index));
            wifi_keys.push_back(index == 0 ? "password" :
                                "password" + std::to_string(index));
        }
        plan.targets.push_back(NvsKeys(
            "wifi-network-profiles", kNetworkProfiles, "", "wifi",
            std::move(wifi_keys), true));
    }

    // The operation key is deliberately last. Core persists the signed erased
    // ACK before invoking FinalizeOwnerState, then this target rotates the
    // operational principal without ever exposing a premature success.
    if (credentials) {
        plan.targets.push_back(NvsKeys(
            "operational-device-identity", kOwnerCredentials, "", "eidolon_id",
            {"p256_priv"}, true, true));
    }
    plan.valid = true;
    return plan;
}

DeviceEraseAdapterOutcome EspIdfDeviceLocalEraseAdapter::StorageOutcome(
    OwnerDataEraseStorageResult result) {
    if (result == OwnerDataEraseStorageResult::PhysicalResetRequired) {
        return {DeviceEraseAdapterResult::PhysicalResetRequired,
                "PHYSICAL_RESET_REQUIRED"};
    }
    return {DeviceEraseAdapterResult::RetryableStorageFailure,
            "RETRYABLE_STORAGE_FAILURE"};
}

DeviceEraseAdapterOutcome EspIdfDeviceLocalEraseAdapter::LoadOrCreateProgress(
    const device_foundation::v1::DeviceLocalEraseCommand& command,
    const OwnerDataErasePlan& plan,
    OwnerDataEraseProgress& progress) {
    const OwnerDataEraseStorageResult loaded = storage_.LoadProgress(progress);
    if (loaded == OwnerDataEraseStorageResult::NotFound) {
        progress.operation_id = command.operation_id;
        progress.plan_id = plan.plan_id;
        progress.target_count = static_cast<uint32_t>(plan.targets.size());
        progress.next_target_index = 0;
        progress.phase = OwnerDataEraseProgressPhase::Preparing;
        const auto stored = storage_.StoreProgress(progress);
        return Success(stored)
                   ? DeviceEraseAdapterOutcome{
                         DeviceEraseAdapterResult::PreparedForFinalization, "PROGRESS_READY"}
                   : StorageOutcome(stored);
    }
    if (!Success(loaded)) return StorageOutcome(loaded);
    if (progress.operation_id != command.operation_id ||
        progress.plan_id != plan.plan_id ||
        progress.target_count != plan.targets.size()) {
        if (progress.phase == OwnerDataEraseProgressPhase::DurableTerminal &&
            progress.operation_id != command.operation_id) {
            progress.operation_id = command.operation_id;
            progress.plan_id = plan.plan_id;
            progress.target_count = static_cast<uint32_t>(plan.targets.size());
            progress.next_target_index = 0;
            progress.phase = OwnerDataEraseProgressPhase::Preparing;
            const auto stored = storage_.StoreProgress(progress);
            return Success(stored)
                       ? DeviceEraseAdapterOutcome{
                             DeviceEraseAdapterResult::PreparedForFinalization,
                             "PROGRESS_READY"}
                       : StorageOutcome(stored);
        }
        return {DeviceEraseAdapterResult::PermanentProtectedScopeRefusal,
                "REMOVAL_OPERATION_CONFLICT"};
    }
    return {DeviceEraseAdapterResult::PreparedForFinalization, "PROGRESS_READY"};
}

DeviceEraseAdapterOutcome EspIdfDeviceLocalEraseAdapter::ProcessTargets(
    const OwnerDataErasePlan& plan,
    OwnerDataEraseProgress& progress,
    uint32_t end_index) {
    while (progress.next_target_index < end_index) {
        const auto& target = plan.targets[progress.next_target_index];
        auto result = storage_.EraseTarget(target);
        if (!Success(result)) return StorageOutcome(result);
        result = storage_.VerifyTargetErased(target);
        if (!Success(result)) return StorageOutcome(result);
        ++progress.next_target_index;
        result = storage_.StoreProgress(progress);
        if (!Success(result)) return StorageOutcome(result);
    }
    return {DeviceEraseAdapterResult::PreparedForFinalization, "PREPARED"};
}

DeviceEraseAdapterOutcome EspIdfDeviceLocalEraseAdapter::PrepareOwnerState(
    const device_foundation::v1::DeviceLocalEraseCommand& command) {
    const OwnerDataErasePlan plan = BuildErasePlan(command);
    if (!plan.valid) {
        return {DeviceEraseAdapterResult::PermanentProtectedScopeRefusal,
                plan.refusal_code};
    }
    OwnerDataEraseProgress progress;
    auto outcome = LoadOrCreateProgress(command, plan, progress);
    if (outcome.result != DeviceEraseAdapterResult::PreparedForFinalization) {
        return outcome;
    }
    if (progress.phase == OwnerDataEraseProgressPhase::DurableTerminal ||
        progress.phase == OwnerDataEraseProgressPhase::Finalizing ||
        progress.phase == OwnerDataEraseProgressPhase::Prepared) {
        return {DeviceEraseAdapterResult::PreparedForFinalization, "PREPARED"};
    }

    uint32_t prepare_count = static_cast<uint32_t>(plan.targets.size());
    while (prepare_count > 0 && plan.targets[prepare_count - 1].finalization_only) {
        --prepare_count;
    }
    outcome = ProcessTargets(plan, progress, prepare_count);
    if (outcome.result != DeviceEraseAdapterResult::PreparedForFinalization) {
        return outcome;
    }
    progress.phase = OwnerDataEraseProgressPhase::Prepared;
    const auto stored = storage_.StoreProgress(progress);
    if (!Success(stored)) return StorageOutcome(stored);
    return {DeviceEraseAdapterResult::PreparedForFinalization, "PREPARED"};
}

DeviceEraseAdapterOutcome EspIdfDeviceLocalEraseAdapter::FinalizeOwnerState(
    const device_foundation::v1::DeviceLocalEraseCommand& command) {
    const OwnerDataErasePlan plan = BuildErasePlan(command);
    if (!plan.valid) {
        return {DeviceEraseAdapterResult::PermanentProtectedScopeRefusal,
                plan.refusal_code};
    }
    OwnerDataEraseProgress progress;
    auto outcome = LoadOrCreateProgress(command, plan, progress);
    if (outcome.result != DeviceEraseAdapterResult::PreparedForFinalization) {
        return outcome;
    }
    if (progress.phase == OwnerDataEraseProgressPhase::DurableTerminal) {
        return {DeviceEraseAdapterResult::Erased, "ERASED"};
    }
    if (progress.phase == OwnerDataEraseProgressPhase::Preparing) {
        return {DeviceEraseAdapterResult::PhysicalResetRequired,
                "FINALIZE_BEFORE_PREPARE"};
    }
    if (progress.phase == OwnerDataEraseProgressPhase::Prepared) {
        progress.phase = OwnerDataEraseProgressPhase::Finalizing;
        const auto stored = storage_.StoreProgress(progress);
        if (!Success(stored)) return StorageOutcome(stored);
    }

    outcome = ProcessTargets(
        plan, progress, static_cast<uint32_t>(plan.targets.size()));
    if (outcome.result != DeviceEraseAdapterResult::PreparedForFinalization) {
        return outcome;
    }
    // A complete verification pass closes the gap between the last per-target
    // checkpoint and the terminal marker. No erased ACK is visible before this.
    for (const auto& target : plan.targets) {
        const auto verified = storage_.VerifyTargetErased(target);
        if (!Success(verified)) return StorageOutcome(verified);
    }
    progress.phase = OwnerDataEraseProgressPhase::DurableTerminal;
    const auto stored = storage_.StoreProgress(progress);
    if (!Success(stored)) return StorageOutcome(stored);
    return {DeviceEraseAdapterResult::Erased, "ERASED"};
}

}  // namespace eidolon
