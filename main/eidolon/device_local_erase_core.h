#ifndef EIDOLON_DEVICE_LOCAL_ERASE_CORE_H_
#define EIDOLON_DEVICE_LOCAL_ERASE_CORE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "device_foundation_v1_generated.h"

namespace eidolon {

enum class DeviceEraseJournalPhase : uint8_t {
    Accepted = 1,
    Staging = 2,
    Erasing = 3,
    DurableTerminal = 4,
};

struct DeviceEraseJournalEntry {
    std::string operation_id;
    std::string request_fingerprint;
    device_foundation::v1::DeviceRef device_ref;
    std::string deadline;
    std::vector<std::string> erase_scopes;
    DeviceEraseJournalPhase phase = DeviceEraseJournalPhase::Accepted;
    // The erased ACK must be signed and durable before the adapter removes the
    // operational signing credential. It is not externally visible until the
    // adapter confirms finalization and phase becomes DurableTerminal.
    bool has_staged_ack = false;
    device_foundation::v1::DeviceLocalEraseAck staged_ack;
};

enum class DeviceEraseJournalLoadResult {
    NotFound,
    Loaded,
    StorageFailure,
};

class DeviceEraseJournalPort {
public:
    virtual ~DeviceEraseJournalPort() = default;
    virtual DeviceEraseJournalLoadResult Load(DeviceEraseJournalEntry& out) = 0;
    virtual bool Store(const DeviceEraseJournalEntry& value) = 0;
};

enum class DeviceEraseAdapterResult {
    PreparedForFinalization,
    Erased,
    RetryableStorageFailure,
    PermanentProtectedScopeRefusal,
    PhysicalResetRequired,
    PermanentFailure,
};

struct DeviceEraseAdapterOutcome {
    DeviceEraseAdapterResult result = DeviceEraseAdapterResult::PermanentFailure;
    std::string result_code;
};

class DeviceLocalEraseAdapterPort {
public:
    virtual ~DeviceLocalEraseAdapterPort() = default;
    // The production implementation belongs to a platform adapter. This Core
    // never calls NVS, Flash, a board singleton or an ESP-IDF erase API.
    // Erases and verifies Owner state except the operational credential used
    // to sign the terminal ACK. Calls are idempotent across power loss.
    virtual DeviceEraseAdapterOutcome PrepareOwnerState(
        const device_foundation::v1::DeviceLocalEraseCommand& command) = 0;
    // Called only after the signed ACK is durable in the RemovalJournal. This
    // removes the retained operational credential and verifies the complete
    // allowlist. Calls are idempotent across power loss.
    virtual DeviceEraseAdapterOutcome FinalizeOwnerState(
        const device_foundation::v1::DeviceLocalEraseCommand& command) = 0;
};

class DeviceEraseClockPort {
public:
    virtual ~DeviceEraseClockPort() = default;
    virtual bool DeadlineExpired(const std::string& rfc3339_deadline) const = 0;
    virtual uint64_t MonotonicTime() const = 0;
};

class DeviceEraseAckSignerPort {
public:
    virtual ~DeviceEraseAckSignerPort() = default;
    virtual bool SignCanonical(const std::string& canonical,
                               std::string& signature) = 0;
};

enum class DeviceEraseCoreResult {
    Acknowledged,
    Replayed,
    IdempotencyConflict,
    StaleGeneration,
    Expired,
    RetryableStorageFailure,
    OperationConflict,
    StorageFailure,
    SignatureFailure,
    NoPendingOperation,
};

struct DeviceEraseCoreOutcome {
    DeviceEraseCoreResult result = DeviceEraseCoreResult::StorageFailure;
    bool has_ack = false;
    device_foundation::v1::DeviceLocalEraseAck ack;
};

class DeviceLocalEraseCore {
public:
    DeviceLocalEraseCore(device_foundation::v1::DeviceRef current_ref,
                         DeviceEraseJournalPort& journal,
                         DeviceLocalEraseAdapterPort& adapter,
                         DeviceEraseClockPort& clock,
                         DeviceEraseAckSignerPort& signer);

    DeviceEraseCoreOutcome Handle(
        const device_foundation::v1::DeviceLocalEraseCommand& command,
        const std::string& request_fingerprint);
    // Boot-time resume uses only the durable RemovalJournal. Delivery does not
    // need to redeliver an operation whose destructive phase already started.
    DeviceEraseCoreOutcome ResumePending();

    static std::string AckSigningDocument(
        const device_foundation::v1::DeviceLocalEraseAck& ack);
    static std::string CommandDocument(
        const device_foundation::v1::DeviceLocalEraseCommand& command);

private:
    static bool SameRef(const device_foundation::v1::DeviceRef& left,
                        const device_foundation::v1::DeviceRef& right);

    device_foundation::v1::DeviceRef current_ref_;
    DeviceEraseJournalPort& journal_;
    DeviceLocalEraseAdapterPort& adapter_;
    DeviceEraseClockPort& clock_;
    DeviceEraseAckSignerPort& signer_;
};

}  // namespace eidolon

#endif  // EIDOLON_DEVICE_LOCAL_ERASE_CORE_H_
