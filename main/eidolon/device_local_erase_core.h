#ifndef EIDOLON_DEVICE_LOCAL_ERASE_CORE_H_
#define EIDOLON_DEVICE_LOCAL_ERASE_CORE_H_

#include <cstdint>
#include <string>

#include "device_foundation_v1_generated.h"

namespace eidolon {

struct DeviceEraseJournalEntry {
    std::string operation_id;
    std::string request_fingerprint;
    device_foundation::v1::DeviceRef device_ref;
    bool terminal = false;
    device_foundation::v1::DeviceLocalEraseAck terminal_ack;
};

class DeviceEraseJournalPort {
public:
    virtual ~DeviceEraseJournalPort() = default;
    virtual bool Load(DeviceEraseJournalEntry& out) = 0;
    virtual bool Store(const DeviceEraseJournalEntry& value) = 0;
};

enum class DeviceEraseAdapterResult {
    Erased,
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
    virtual DeviceEraseAdapterOutcome EraseOwnerState(
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
    StorageFailure,
    SignatureFailure,
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

    static std::string AckSigningDocument(
        const device_foundation::v1::DeviceLocalEraseAck& ack);

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
