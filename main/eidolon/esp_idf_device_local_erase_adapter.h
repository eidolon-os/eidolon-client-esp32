#ifndef EIDOLON_ESP_IDF_DEVICE_LOCAL_ERASE_ADAPTER_H_
#define EIDOLON_ESP_IDF_DEVICE_LOCAL_ERASE_ADAPTER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "device_local_erase_core.h"

namespace eidolon {

enum class OwnerDataEraseTargetKind : uint8_t {
    NvsKeys = 1,
    DataPartition = 2,
};

struct OwnerDataEraseTarget {
    std::string target_id;
    std::string erase_scope;
    OwnerDataEraseTargetKind kind = OwnerDataEraseTargetKind::NvsKeys;
    // Empty means the default NVS partition.
    std::string partition_label;
    std::string nvs_namespace;
    std::vector<std::string> keys;
    bool optional = false;
    bool finalization_only = false;
};

struct OwnerDataErasePlan {
    bool valid = false;
    std::string refusal_code;
    std::string plan_id;
    std::vector<OwnerDataEraseTarget> targets;
};

enum class OwnerDataEraseProgressPhase : uint8_t {
    Preparing = 1,
    Prepared = 2,
    Finalizing = 3,
    DurableTerminal = 4,
};

struct OwnerDataEraseProgress {
    std::string operation_id;
    std::string plan_id;
    uint32_t target_count = 0;
    uint32_t next_target_index = 0;
    OwnerDataEraseProgressPhase phase = OwnerDataEraseProgressPhase::Preparing;
};

enum class OwnerDataEraseStorageResult {
    Done,
    NotFound,
    RetryableFailure,
    PhysicalResetRequired,
};

class OwnerDataEraseStoragePort {
public:
    virtual ~OwnerDataEraseStoragePort() = default;
    virtual OwnerDataEraseStorageResult LoadProgress(
        OwnerDataEraseProgress& out) = 0;
    virtual OwnerDataEraseStorageResult StoreProgress(
        const OwnerDataEraseProgress& value) = 0;
    virtual OwnerDataEraseStorageResult EraseTarget(
        const OwnerDataEraseTarget& target) = 0;
    virtual OwnerDataEraseStorageResult VerifyTargetErased(
        const OwnerDataEraseTarget& target) = 0;
};

// Production policy adapter. The policy and resume engine are host-testable;
// EspIdfOwnerDataEraseStorage is the only class that touches ESP-IDF NVS/Flash.
class EspIdfDeviceLocalEraseAdapter final : public DeviceLocalEraseAdapterPort {
public:
    explicit EspIdfDeviceLocalEraseAdapter(OwnerDataEraseStoragePort& storage)
        : storage_(storage) {}

    DeviceEraseAdapterOutcome PrepareOwnerState(
        const device_foundation::v1::DeviceLocalEraseCommand& command) override;
    DeviceEraseAdapterOutcome FinalizeOwnerState(
        const device_foundation::v1::DeviceLocalEraseCommand& command) override;

    static OwnerDataErasePlan BuildErasePlan(
        const device_foundation::v1::DeviceLocalEraseCommand& command);
    static const std::vector<std::string>& ProtectedNvsNamespaces();
    static const std::vector<std::string>& ProtectedPartitions();

private:
    DeviceEraseAdapterOutcome LoadOrCreateProgress(
        const device_foundation::v1::DeviceLocalEraseCommand& command,
        const OwnerDataErasePlan& plan,
        OwnerDataEraseProgress& progress);
    DeviceEraseAdapterOutcome ProcessTargets(
        const OwnerDataErasePlan& plan,
        OwnerDataEraseProgress& progress,
        uint32_t end_index);
    static DeviceEraseAdapterOutcome StorageOutcome(
        OwnerDataEraseStorageResult result);

    OwnerDataEraseStoragePort& storage_;
};

}  // namespace eidolon

#endif  // EIDOLON_ESP_IDF_DEVICE_LOCAL_ERASE_ADAPTER_H_
