#include "eidolon/esp_idf_device_local_erase_adapter.h"

#include <algorithm>
#include <cassert>
#include <set>
#include <string>
#include <vector>

using eidolon::DeviceEraseAdapterResult;
using eidolon::EspIdfDeviceLocalEraseAdapter;
using eidolon::OwnerDataEraseProgress;
using eidolon::OwnerDataEraseProgressPhase;
using eidolon::OwnerDataEraseStoragePort;
using eidolon::OwnerDataEraseStorageResult;
using eidolon::OwnerDataEraseTarget;
using eidolon::OwnerDataEraseTargetKind;
using eidolon::device_foundation::v1::DeviceLocalEraseCommand;
using eidolon::device_foundation::v1::DeviceRef;

namespace {

DeviceRef Ref(uint32_t generation = 7) {
    return DeviceRef{
        "device_erase_01",
        {"owner-domain_01"},
        3,
        generation,
        4,
    };
}

DeviceLocalEraseCommand Command() {
    return DeviceLocalEraseCommand{
        "erase_operation_01",
        Ref(),
        "2026-08-30T00:00:00Z",
        {"owner-credentials", "owner-data", "network-profiles"},
    };
}

class Storage final : public OwnerDataEraseStoragePort {
public:
    OwnerDataEraseStorageResult LoadProgress(
        OwnerDataEraseProgress& out) override {
        if (!present) return OwnerDataEraseStorageResult::NotFound;
        out = progress;
        return OwnerDataEraseStorageResult::Done;
    }

    OwnerDataEraseStorageResult StoreProgress(
        const OwnerDataEraseProgress& value) override {
        ++store_calls;
        if (fail_store_call == store_calls) {
            return OwnerDataEraseStorageResult::RetryableFailure;
        }
        progress = value;
        present = true;
        return OwnerDataEraseStorageResult::Done;
    }

    OwnerDataEraseStorageResult EraseTarget(
        const OwnerDataEraseTarget& target) override {
        ++erase_calls;
        erase_order.push_back(target.target_id);
        if (fail_erase_call == erase_calls) {
            return fail_result;
        }
        erased.insert(target.target_id);
        return OwnerDataEraseStorageResult::Done;
    }

    OwnerDataEraseStorageResult VerifyTargetErased(
        const OwnerDataEraseTarget& target) override {
        ++verify_calls;
        if (fail_verify_call == verify_calls) {
            return fail_result;
        }
        return erased.count(target.target_id) != 0
                   ? OwnerDataEraseStorageResult::Done
                   : OwnerDataEraseStorageResult::RetryableFailure;
    }

    bool present = false;
    int store_calls = 0;
    int erase_calls = 0;
    int verify_calls = 0;
    int fail_store_call = -1;
    int fail_erase_call = -1;
    int fail_verify_call = -1;
    OwnerDataEraseStorageResult fail_result =
        OwnerDataEraseStorageResult::RetryableFailure;
    OwnerDataEraseProgress progress;
    std::set<std::string> erased;
    std::vector<std::string> erase_order;
};

bool Contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

void ExactAllowlistPreservesFactoryAndCalibrationState() {
    const auto plan = EspIdfDeviceLocalEraseAdapter::BuildErasePlan(Command());
    assert(plan.valid);
    assert(!plan.targets.empty());
    std::vector<std::string> ids;
    for (const auto& target : plan.targets) ids.push_back(target.target_id);

    assert(Contains(ids, "owner-trust-dedicated"));
    assert(Contains(ids, "active-claim-and-onboarding"));
    assert(Contains(ids, "wifi-network-profiles"));
    assert(Contains(ids, "owner-face-db"));
    assert(Contains(ids, "operational-device-identity"));
    assert(plan.targets.back().target_id == "operational-device-identity");
    assert(plan.targets.back().finalization_only);

    for (const char* protected_partition :
         {"nvsfactory", "nvs_keys", "phy_init", "assets", "assets_A",
          "human_face_det", "human_face_feat", "model", "otadata",
          "ota_0", "ota_1", "factory"}) {
        assert(Contains(EspIdfDeviceLocalEraseAdapter::ProtectedPartitions(),
                        protected_partition));
    }

    for (const auto& protected_namespace :
         EspIdfDeviceLocalEraseAdapter::ProtectedNvsNamespaces()) {
        for (const auto& target : plan.targets) {
            assert(target.nvs_namespace != protected_namespace);
        }
    }
    for (const auto& protected_partition :
         EspIdfDeviceLocalEraseAdapter::ProtectedPartitions()) {
        for (const auto& target : plan.targets) {
            assert(target.partition_label != protected_partition);
        }
    }
}

void PrepareAndFinalizeAreDurableAndIdempotent() {
    Storage storage;
    EspIdfDeviceLocalEraseAdapter adapter(storage);
    const auto prepared = adapter.PrepareOwnerState(Command());
    assert(prepared.result == DeviceEraseAdapterResult::PreparedForFinalization);
    assert(storage.progress.phase == OwnerDataEraseProgressPhase::Prepared);
    assert(storage.erased.count("operational-device-identity") == 0);

    const auto finalized = adapter.FinalizeOwnerState(Command());
    assert(finalized.result == DeviceEraseAdapterResult::Erased);
    assert(storage.progress.phase == OwnerDataEraseProgressPhase::DurableTerminal);
    assert(storage.erased.count("operational-device-identity") == 1);
    const int erase_calls = storage.erase_calls;
    assert(adapter.FinalizeOwnerState(Command()).result ==
           DeviceEraseAdapterResult::Erased);
    assert(storage.erase_calls == erase_calls);
}

void RetryablePartialFailureResumesSameTargetWithoutAckOutcome() {
    Storage storage;
    storage.fail_erase_call = 3;
    EspIdfDeviceLocalEraseAdapter adapter(storage);
    const auto first = adapter.PrepareOwnerState(Command());
    assert(first.result == DeviceEraseAdapterResult::RetryableStorageFailure);
    const uint32_t checkpoint = storage.progress.next_target_index;
    assert(checkpoint == 2);

    storage.fail_erase_call = -1;
    const auto resumed = adapter.PrepareOwnerState(Command());
    assert(resumed.result == DeviceEraseAdapterResult::PreparedForFinalization);
    assert(storage.erase_order[2] == storage.erase_order[3]);
}

void EveryEraseCheckpointCanResumeAfterPowerLoss() {
    const auto plan = EspIdfDeviceLocalEraseAdapter::BuildErasePlan(Command());
    assert(plan.valid);
    for (int failure = 1; failure <= static_cast<int>(plan.targets.size()); ++failure) {
        Storage storage;
        storage.fail_erase_call = failure;
        EspIdfDeviceLocalEraseAdapter adapter(storage);
        auto outcome = adapter.PrepareOwnerState(Command());
        if (outcome.result == DeviceEraseAdapterResult::PreparedForFinalization) {
            outcome = adapter.FinalizeOwnerState(Command());
        }
        assert(outcome.result == DeviceEraseAdapterResult::RetryableStorageFailure);

        storage.fail_erase_call = -1;
        assert(adapter.PrepareOwnerState(Command()).result ==
               DeviceEraseAdapterResult::PreparedForFinalization);
        assert(adapter.FinalizeOwnerState(Command()).result ==
               DeviceEraseAdapterResult::Erased);
        assert(storage.progress.phase ==
               OwnerDataEraseProgressPhase::DurableTerminal);
    }
}

void EveryProgressAndVerificationCheckpointCanResumeAfterPowerLoss() {
    Storage baseline;
    EspIdfDeviceLocalEraseAdapter baseline_adapter(baseline);
    assert(baseline_adapter.PrepareOwnerState(Command()).result ==
           DeviceEraseAdapterResult::PreparedForFinalization);
    assert(baseline_adapter.FinalizeOwnerState(Command()).result ==
           DeviceEraseAdapterResult::Erased);
    const int store_checkpoints = baseline.store_calls;
    const int verify_checkpoints = baseline.verify_calls;

    for (int failure = 1; failure <= store_checkpoints; ++failure) {
        Storage storage;
        storage.fail_store_call = failure;
        EspIdfDeviceLocalEraseAdapter adapter(storage);
        auto outcome = adapter.PrepareOwnerState(Command());
        if (outcome.result == DeviceEraseAdapterResult::PreparedForFinalization) {
            outcome = adapter.FinalizeOwnerState(Command());
        }
        assert(outcome.result == DeviceEraseAdapterResult::RetryableStorageFailure);
        storage.fail_store_call = -1;
        assert(adapter.PrepareOwnerState(Command()).result ==
               DeviceEraseAdapterResult::PreparedForFinalization);
        assert(adapter.FinalizeOwnerState(Command()).result ==
               DeviceEraseAdapterResult::Erased);
    }

    for (int failure = 1; failure <= verify_checkpoints; ++failure) {
        Storage storage;
        storage.fail_verify_call = failure;
        EspIdfDeviceLocalEraseAdapter adapter(storage);
        auto outcome = adapter.PrepareOwnerState(Command());
        if (outcome.result == DeviceEraseAdapterResult::PreparedForFinalization) {
            outcome = adapter.FinalizeOwnerState(Command());
        }
        assert(outcome.result == DeviceEraseAdapterResult::RetryableStorageFailure);
        storage.fail_verify_call = -1;
        assert(adapter.PrepareOwnerState(Command()).result ==
               DeviceEraseAdapterResult::PreparedForFinalization);
        assert(adapter.FinalizeOwnerState(Command()).result ==
               DeviceEraseAdapterResult::Erased);
    }
}

void UnknownOrFactoryScopeIsPermanentlyRefused() {
    Storage storage;
    EspIdfDeviceLocalEraseAdapter adapter(storage);
    auto command = Command();
    command.erase_scopes.push_back("factory-calibration");
    const auto refused = adapter.PrepareOwnerState(command);
    assert(refused.result ==
           DeviceEraseAdapterResult::PermanentProtectedScopeRefusal);
    assert(refused.result_code == "PROTECTED_FACTORY_SCOPE_REFUSED");
    assert(storage.erase_calls == 0);
    assert(!storage.present);
}

void DamagedOrUnusableStorageRequiresPhysicalReset() {
    Storage storage;
    storage.fail_erase_call = 1;
    storage.fail_result = OwnerDataEraseStorageResult::PhysicalResetRequired;
    EspIdfDeviceLocalEraseAdapter adapter(storage);
    const auto failed = adapter.PrepareOwnerState(Command());
    assert(failed.result == DeviceEraseAdapterResult::PhysicalResetRequired);
    assert(failed.result_code == "PHYSICAL_RESET_REQUIRED");
}

void DifferentOperationCannotTakeOverDurableProgress() {
    Storage storage;
    storage.fail_erase_call = 2;
    EspIdfDeviceLocalEraseAdapter adapter(storage);
    assert(adapter.PrepareOwnerState(Command()).result ==
           DeviceEraseAdapterResult::RetryableStorageFailure);
    auto other = Command();
    other.operation_id = "erase_operation_02";
    const auto refused = adapter.PrepareOwnerState(other);
    assert(refused.result ==
           DeviceEraseAdapterResult::PermanentProtectedScopeRefusal);
    assert(refused.result_code == "REMOVAL_OPERATION_CONFLICT");
}

void FreshOperationCanReplaceOnlyDurableTerminalProgress() {
    Storage storage;
    EspIdfDeviceLocalEraseAdapter adapter(storage);
    assert(adapter.PrepareOwnerState(Command()).result ==
           DeviceEraseAdapterResult::PreparedForFinalization);
    assert(adapter.FinalizeOwnerState(Command()).result ==
           DeviceEraseAdapterResult::Erased);

    auto next = Command();
    next.operation_id = "erase_operation_02";
    assert(adapter.PrepareOwnerState(next).result ==
           DeviceEraseAdapterResult::PreparedForFinalization);
    assert(storage.progress.operation_id == "erase_operation_02");
}

}  // namespace

int main() {
    ExactAllowlistPreservesFactoryAndCalibrationState();
    PrepareAndFinalizeAreDurableAndIdempotent();
    RetryablePartialFailureResumesSameTargetWithoutAckOutcome();
    EveryEraseCheckpointCanResumeAfterPowerLoss();
    EveryProgressAndVerificationCheckpointCanResumeAfterPowerLoss();
    UnknownOrFactoryScopeIsPermanentlyRefused();
    DamagedOrUnusableStorageRequiresPhysicalReset();
    DifferentOperationCannotTakeOverDurableProgress();
    FreshOperationCanReplaceOnlyDurableTerminalProgress();
    return 0;
}
