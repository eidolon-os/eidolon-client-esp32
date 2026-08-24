#ifndef EIDOLON_ESP_IDF_OWNER_DATA_ERASE_STORAGE_H_
#define EIDOLON_ESP_IDF_OWNER_DATA_ERASE_STORAGE_H_

#include "esp_idf_device_local_erase_adapter.h"

namespace eidolon {

inline constexpr const char* kDeviceEraseJournalNvsNamespace = "eid_erase";

// ESP-IDF production backend. It erases only targets from the adapter's static
// allowlist and never calls nvs_flash_erase/erase_flash.
class EspIdfOwnerDataEraseStorage final : public OwnerDataEraseStoragePort {
public:
    OwnerDataEraseStorageResult LoadProgress(
        OwnerDataEraseProgress& out) override;
    OwnerDataEraseStorageResult StoreProgress(
        const OwnerDataEraseProgress& value) override;
    OwnerDataEraseStorageResult EraseTarget(
        const OwnerDataEraseTarget& target) override;
    OwnerDataEraseStorageResult VerifyTargetErased(
        const OwnerDataEraseTarget& target) override;

private:
    static OwnerDataEraseStorageResult Classify(int error);
};

// Durable Core/RemovalJournal stored outside every Owner-data target. Core and
// adapter progress use independent CRC-checked, sequence-numbered alternating
// blobs; a torn new slot leaves the previous slot resumable. If neither slot is
// valid, load fails closed so a second operation cannot overwrite partial erase.
class EspIdfDeviceEraseJournal final : public DeviceEraseJournalPort {
public:
    DeviceEraseJournalLoadResult Load(DeviceEraseJournalEntry& out) override;
    bool Store(const DeviceEraseJournalEntry& value) override;
};

}  // namespace eidolon

#endif  // EIDOLON_ESP_IDF_OWNER_DATA_ERASE_STORAGE_H_
