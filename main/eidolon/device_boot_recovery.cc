#include "device_boot_recovery.h"

#include "device_erase_identity_adapter.h"
#include "esp_idf_device_local_erase_adapter.h"
#include "esp_idf_owner_data_erase_storage.h"

#include <esp_timer.h>

namespace eidolon {
namespace {

class DestructiveResumeClock final : public DeviceEraseClockPort {
public:
    // Boot recovery has no trusted wall clock. Accepted/Staging checkpoints
    // therefore fail closed as expired and require authenticated redelivery.
    // DeviceLocalEraseCore ignores this answer only after the durable Erasing
    // point of no return, where forward resume is mandatory.
    bool DeadlineExpired(const std::string&) const override { return true; }
    uint64_t MonotonicTime() const override {
        return static_cast<uint64_t>(esp_timer_get_time() / 1000);
    }
};

}  // namespace

DeviceEraseCoreOutcome DeviceBootRecovery::ResumePendingRemoval() {
    EspIdfDeviceEraseJournal journal;
    DeviceEraseJournalEntry entry;
    const DeviceEraseJournalLoadResult loaded = journal.Load(entry);
    if (loaded == DeviceEraseJournalLoadResult::NotFound) {
        return {DeviceEraseCoreResult::NoPendingOperation, false, {}};
    }
    if (loaded == DeviceEraseJournalLoadResult::StorageFailure) {
        return {DeviceEraseCoreResult::StorageFailure, false, {}};
    }

    EspIdfOwnerDataEraseStorage storage;
    EspIdfDeviceLocalEraseAdapter adapter(storage);
    DestructiveResumeClock clock;
    DeviceIdentityEraseAckSigner signer;
    DeviceLocalEraseCore core(entry.device_ref, journal, adapter, clock, signer);
    return core.ResumePending();
}

}  // namespace eidolon
