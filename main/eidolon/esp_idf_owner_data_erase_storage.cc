#include "esp_idf_owner_data_erase_storage.h"

#include "device_ref_access.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include <esp_err.h>
#include <esp_partition.h>
#include <nvs.h>

namespace eidolon {
namespace {

constexpr uint8_t kProgressSchema = 1;
constexpr uint8_t kCoreJournalSchema = 1;
constexpr uint32_t kProgressMagic = 0x45504150;  // EPAP
constexpr uint32_t kCoreJournalMagic = 0x45434a52;  // ECJR
constexpr size_t kMaxJournalBlobSize = 16 * 1024;
constexpr const char* kProgressSlots[] = {"ap0", "ap1"};
constexpr const char* kCoreJournalSlots[] = {"cj0", "cj1"};

uint32_t Crc32(const uint8_t* data, size_t length) {
    uint32_t crc = std::numeric_limits<uint32_t>::max();
    for (size_t index = 0; index < length; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

void AppendU8(std::vector<uint8_t>& out, uint8_t value) {
    out.push_back(value);
}

void AppendU32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<uint8_t>(value >> shift));
    }
}

void AppendU64(std::vector<uint8_t>& out, uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<uint8_t>(value >> shift));
    }
}

bool AppendString(std::vector<uint8_t>& out, const std::string& value) {
    if (value.size() > kMaxJournalBlobSize) return false;
    AppendU32(out, static_cast<uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
    return out.size() <= kMaxJournalBlobSize;
}

class BlobReader {
public:
    BlobReader() = default;
    BlobReader(const std::vector<uint8_t>& value, size_t payload_end)
        : value_(&value), payload_end_(payload_end) {}

    bool U8(uint8_t& out) {
        if (offset_ + 1 > payload_end_) return false;
        out = (*value_)[offset_++];
        return true;
    }

    bool U32(uint32_t& out) {
        if (offset_ + 4 > payload_end_) return false;
        out = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            out |= static_cast<uint32_t>((*value_)[offset_++]) << shift;
        }
        return true;
    }

    bool U64(uint64_t& out) {
        if (offset_ + 8 > payload_end_) return false;
        out = 0;
        for (int shift = 0; shift < 64; shift += 8) {
            out |= static_cast<uint64_t>((*value_)[offset_++]) << shift;
        }
        return true;
    }

    bool String(std::string& out) {
        uint32_t length = 0;
        if (!U32(length) || length > payload_end_ - offset_) return false;
        out.assign(reinterpret_cast<const char*>(value_->data() + offset_), length);
        offset_ += length;
        return true;
    }

    bool AtEnd() const { return offset_ == payload_end_; }

private:
    const std::vector<uint8_t>* value_ = nullptr;
    size_t payload_end_ = 0;
    size_t offset_ = 0;
};

void FinishBlob(std::vector<uint8_t>& blob) {
    AppendU32(blob, Crc32(blob.data(), blob.size()));
}

bool BeginBlobRead(const std::vector<uint8_t>& blob, uint32_t expected_magic,
                   uint8_t expected_schema, BlobReader& reader,
                   uint64_t& sequence) {
    if (blob.size() < 4 + 1 + 8 + 4) return false;
    const size_t payload_end = blob.size() - 4;
    uint32_t stored_crc = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        stored_crc |= static_cast<uint32_t>(blob[payload_end + shift / 8]) << shift;
    }
    if (stored_crc != Crc32(blob.data(), payload_end)) return false;
    reader = BlobReader(blob, payload_end);
    uint32_t magic = 0;
    uint8_t schema = 0;
    return reader.U32(magic) && reader.U8(schema) && reader.U64(sequence) &&
           magic == expected_magic && schema == expected_schema && sequence != 0;
}

esp_err_t ReadBlob(nvs_handle_t handle, const char* key,
                   std::vector<uint8_t>& value, bool& exists) {
    exists = false;
    size_t size = 0;
    esp_err_t err = nvs_get_blob(handle, key, nullptr, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    exists = true;
    if (size == 0 || size > kMaxJournalBlobSize) return ESP_ERR_INVALID_SIZE;
    value.resize(size);
    return nvs_get_blob(handle, key, value.data(), &size);
}

struct StoredBlob {
    bool exists = false;
    bool valid = false;
    uint64_t sequence = 0;
    std::vector<uint8_t> bytes;
};

esp_err_t OpenTarget(const OwnerDataEraseTarget& target, nvs_open_mode_t mode,
                     nvs_handle_t& handle) {
    if (target.partition_label.empty()) {
        return nvs_open(target.nvs_namespace.c_str(), mode, &handle);
    }
    return nvs_open_from_partition(target.partition_label.c_str(),
                                   target.nvs_namespace.c_str(), mode, &handle);
}

bool IsTargetKey(const OwnerDataEraseTarget& target, const char* key) {
    return std::find(target.keys.begin(), target.keys.end(), key) !=
           target.keys.end();
}

const char* NvsPartitionName(const OwnerDataEraseTarget& target) {
    return target.partition_label.empty() ? "nvs" : target.partition_label.c_str();
}

esp_err_t FirstTargetEntry(const OwnerDataEraseTarget& target,
                           nvs_iterator_t& iterator) {
    iterator = nullptr;
    esp_err_t result = nvs_entry_find(
        NvsPartitionName(target), target.nvs_namespace.c_str(),
        NVS_TYPE_ANY, &iterator);
    while (result == ESP_OK) {
        nvs_entry_info_t info{};
        result = nvs_entry_info(iterator, &info);
        if (result != ESP_OK || IsTargetKey(target, info.key)) return result;
        result = nvs_entry_next(&iterator);
    }
    return result;
}

bool ValidProgressPhase(uint8_t phase) {
    return phase >= static_cast<uint8_t>(OwnerDataEraseProgressPhase::Preparing) &&
           phase <= static_cast<uint8_t>(OwnerDataEraseProgressPhase::DurableTerminal);
}

bool ValidCorePhase(uint8_t phase) {
    return phase >= static_cast<uint8_t>(DeviceEraseJournalPhase::Accepted) &&
           phase <= static_cast<uint8_t>(DeviceEraseJournalPhase::DurableTerminal);
}

bool EncodeProgress(const OwnerDataEraseProgress& value, uint64_t sequence,
                    std::vector<uint8_t>& blob) {
    if (value.operation_id.empty() || value.plan_id.empty() ||
        value.target_count == 0 || value.next_target_index > value.target_count ||
        !ValidProgressPhase(static_cast<uint8_t>(value.phase))) {
        return false;
    }
    blob.clear();
    AppendU32(blob, kProgressMagic);
    AppendU8(blob, kProgressSchema);
    AppendU64(blob, sequence);
    if (!AppendString(blob, value.operation_id) ||
        !AppendString(blob, value.plan_id)) {
        return false;
    }
    AppendU32(blob, value.target_count);
    AppendU32(blob, value.next_target_index);
    AppendU8(blob, static_cast<uint8_t>(value.phase));
    FinishBlob(blob);
    return blob.size() <= kMaxJournalBlobSize;
}

bool DecodeProgress(const std::vector<uint8_t>& blob,
                    OwnerDataEraseProgress& out, uint64_t& sequence) {
    BlobReader reader;
    if (!BeginBlobRead(blob, kProgressMagic, kProgressSchema, reader, sequence)) {
        return false;
    }
    uint8_t phase = 0;
    OwnerDataEraseProgress decoded;
    if (!reader.String(decoded.operation_id) || !reader.String(decoded.plan_id) ||
        !reader.U32(decoded.target_count) ||
        !reader.U32(decoded.next_target_index) || !reader.U8(phase) ||
        !reader.AtEnd() || decoded.operation_id.empty() ||
        decoded.plan_id.empty() || decoded.target_count == 0 ||
        decoded.next_target_index > decoded.target_count ||
        !ValidProgressPhase(phase)) {
        return false;
    }
    decoded.phase = static_cast<OwnerDataEraseProgressPhase>(phase);
    out = std::move(decoded);
    return true;
}

bool ValidCoreJournal(const DeviceEraseJournalEntry& value) {
    if (value.operation_id.empty() || value.request_fingerprint.empty() ||
        value.deadline.empty() || value.erase_scopes.empty() ||
        value.device_ref.device_instance_id.empty() ||
        DeviceRefOwnerDomainId(value.device_ref).empty() ||
        !ValidCorePhase(static_cast<uint8_t>(value.phase)) ||
        (value.phase == DeviceEraseJournalPhase::DurableTerminal &&
         !value.has_staged_ack) ||
        (value.has_staged_ack &&
         value.phase != DeviceEraseJournalPhase::Erasing &&
         value.phase != DeviceEraseJournalPhase::DurableTerminal)) {
        return false;
    }
    if (!value.has_staged_ack) return true;
    return value.staged_ack.operation_id == value.operation_id &&
           SameDeviceRef(value.staged_ack.device_ref, value.device_ref) &&
           value.staged_ack.ack_sequence != 0 &&
           !value.staged_ack.result_code.empty() &&
           !value.staged_ack.device_signature.empty();
}

bool EncodeCoreJournal(const DeviceEraseJournalEntry& value, uint64_t sequence,
                       std::vector<uint8_t>& blob) {
    if (!ValidCoreJournal(value) || value.erase_scopes.size() > 16) return false;
    blob.clear();
    AppendU32(blob, kCoreJournalMagic);
    AppendU8(blob, kCoreJournalSchema);
    AppendU64(blob, sequence);
    if (!AppendString(blob, value.operation_id) ||
        !AppendString(blob, value.request_fingerprint) ||
        !AppendString(blob, value.deadline)) {
        return false;
    }
    AppendU32(blob, static_cast<uint32_t>(value.erase_scopes.size()));
    for (const auto& scope : value.erase_scopes) {
        if (scope.empty() || !AppendString(blob, scope)) return false;
    }
    if (!AppendString(blob, value.device_ref.device_instance_id) ||
        !AppendString(blob, DeviceRefOwnerDomainId(value.device_ref))) {
        return false;
    }
    AppendU64(blob, value.device_ref.owner_domain_generation);
    AppendU32(blob, value.device_ref.claim_generation);
    AppendU32(blob, value.device_ref.trust_epoch);
    if (!AppendString(blob, DeviceRefLegacyManifestDigest(value.device_ref))) {
        return false;
    }
    AppendU8(blob, static_cast<uint8_t>(value.phase));
    AppendU8(blob, value.has_staged_ack ? 1 : 0);
    if (value.has_staged_ack) {
        AppendU32(blob, value.staged_ack.ack_sequence);
        AppendU8(blob,
                 value.staged_ack.result ==
                         device_foundation::v1::DeviceLocalEraseResult::Erased
                     ? 1
                     : 2);
        if (!AppendString(blob, value.staged_ack.result_code)) return false;
        AppendU64(blob, value.staged_ack.device_monotonic_time);
        if (!AppendString(blob, value.staged_ack.device_signature)) return false;
    }
    FinishBlob(blob);
    return blob.size() <= kMaxJournalBlobSize;
}

bool DecodeCoreJournal(const std::vector<uint8_t>& blob,
                       DeviceEraseJournalEntry& out, uint64_t& sequence) {
    BlobReader reader;
    if (!BeginBlobRead(blob, kCoreJournalMagic, kCoreJournalSchema,
                       reader, sequence)) {
        return false;
    }
    DeviceEraseJournalEntry decoded;
    uint32_t scope_count = 0;
    std::string owner_domain_id;
    std::string legacy_manifest;
    uint8_t phase = 0;
    uint8_t has_ack = 0;
    if (!reader.String(decoded.operation_id) ||
        !reader.String(decoded.request_fingerprint) ||
        !reader.String(decoded.deadline) || !reader.U32(scope_count) ||
        scope_count == 0 || scope_count > 16) {
        return false;
    }
    decoded.erase_scopes.reserve(scope_count);
    for (uint32_t index = 0; index < scope_count; ++index) {
        std::string scope;
        if (!reader.String(scope) || scope.empty()) return false;
        decoded.erase_scopes.push_back(std::move(scope));
    }
    if (!reader.String(decoded.device_ref.device_instance_id) ||
        !reader.String(owner_domain_id) ||
        !reader.U64(decoded.device_ref.owner_domain_generation) ||
        !reader.U32(decoded.device_ref.claim_generation) ||
        !reader.U32(decoded.device_ref.trust_epoch) ||
        !reader.String(legacy_manifest) || !reader.U8(phase) ||
        !reader.U8(has_ack) || has_ack > 1 || !ValidCorePhase(phase)) {
        return false;
    }
    decoded.phase = static_cast<DeviceEraseJournalPhase>(phase);
    decoded.has_staged_ack = has_ack != 0;
    SetDeviceRefOwnerDomainId(decoded.device_ref, owner_domain_id);
    SetDeviceRefLegacyManifestDigest(decoded.device_ref, legacy_manifest);
    if (decoded.has_staged_ack) {
        uint8_t ack_result = 0;
        decoded.staged_ack.operation_id = decoded.operation_id;
        decoded.staged_ack.device_ref = decoded.device_ref;
        if (!reader.U32(decoded.staged_ack.ack_sequence) ||
            !reader.U8(ack_result) || (ack_result != 1 && ack_result != 2) ||
            !reader.String(decoded.staged_ack.result_code) ||
            !reader.U64(decoded.staged_ack.device_monotonic_time) ||
            !reader.String(decoded.staged_ack.device_signature)) {
            return false;
        }
        decoded.staged_ack.result =
            ack_result == 1
                ? device_foundation::v1::DeviceLocalEraseResult::Erased
                : device_foundation::v1::DeviceLocalEraseResult::PermanentFailure;
    }
    if (!reader.AtEnd() || !ValidCoreJournal(decoded)) return false;
    out = std::move(decoded);
    return true;
}

template <typename Value, typename Decoder>
esp_err_t ReadSlots(nvs_handle_t handle, const char* const (&slots)[2],
                    Decoder decode, std::array<StoredBlob, 2>& candidates,
                    std::array<Value, 2>& decoded) {
    for (size_t index = 0; index < candidates.size(); ++index) {
        esp_err_t err = ReadBlob(handle, slots[index], candidates[index].bytes,
                                 candidates[index].exists);
        if (err != ESP_OK) return err;
        if (candidates[index].exists) {
            candidates[index].valid = decode(candidates[index].bytes,
                                               decoded[index],
                                               candidates[index].sequence);
        }
    }
    return ESP_OK;
}

int NewestValidSlot(const std::array<StoredBlob, 2>& candidates) {
    if (candidates[0].valid && candidates[1].valid) {
        if (candidates[0].sequence == candidates[1].sequence &&
            candidates[0].bytes != candidates[1].bytes) {
            return -1;
        }
        return candidates[1].sequence > candidates[0].sequence ? 1 : 0;
    }
    if (candidates[0].valid) return 0;
    if (candidates[1].valid) return 1;
    return -1;
}

bool AnySlotExists(const std::array<StoredBlob, 2>& candidates) {
    return candidates[0].exists || candidates[1].exists;
}

}  // namespace

OwnerDataEraseStorageResult EspIdfOwnerDataEraseStorage::Classify(int error) {
    const esp_err_t err = static_cast<esp_err_t>(error);
    if (err == ESP_OK) return OwnerDataEraseStorageResult::Done;
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_PART_NOT_FOUND ||
        err == ESP_ERR_NOT_FOUND) {
        return OwnerDataEraseStorageResult::NotFound;
    }
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE ||
        err == ESP_ERR_NVS_INVALID_NAME || err == ESP_ERR_NVS_INVALID_LENGTH ||
        err == ESP_ERR_NVS_CORRUPT_KEY_PART ||
        err == ESP_ERR_NVS_WRONG_ENCRYPTION ||
        err == ESP_ERR_NVS_XTS_CFG_FAILED ||
        err == ESP_ERR_NVS_KEYS_NOT_INITIALIZED) {
        return OwnerDataEraseStorageResult::PhysicalResetRequired;
    }
    return OwnerDataEraseStorageResult::RetryableFailure;
}

OwnerDataEraseStorageResult EspIdfOwnerDataEraseStorage::LoadProgress(
    OwnerDataEraseProgress& out) {
    out = {};
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kDeviceEraseJournalNvsNamespace,
                             NVS_READONLY, &handle);
    if (err != ESP_OK) return Classify(err);
    std::array<StoredBlob, 2> candidates;
    std::array<OwnerDataEraseProgress, 2> decoded;
    err = ReadSlots(handle, kProgressSlots, DecodeProgress,
                    candidates, decoded);
    nvs_close(handle);
    if (err != ESP_OK) return Classify(err);
    const int newest = NewestValidSlot(candidates);
    if (newest < 0) {
        if (!AnySlotExists(candidates)) return OwnerDataEraseStorageResult::NotFound;
        return OwnerDataEraseStorageResult::PhysicalResetRequired;
    }
    out = std::move(decoded[static_cast<size_t>(newest)]);
    return OwnerDataEraseStorageResult::Done;
}

OwnerDataEraseStorageResult EspIdfOwnerDataEraseStorage::StoreProgress(
    const OwnerDataEraseProgress& value) {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kDeviceEraseJournalNvsNamespace,
                             NVS_READWRITE, &handle);
    if (err != ESP_OK) return Classify(err);
    std::array<StoredBlob, 2> candidates;
    std::array<OwnerDataEraseProgress, 2> decoded;
    err = ReadSlots(handle, kProgressSlots, DecodeProgress,
                    candidates, decoded);
    const int newest = err == ESP_OK ? NewestValidSlot(candidates) : -1;
    if (err == ESP_OK && newest < 0 && AnySlotExists(candidates)) {
        nvs_close(handle);
        return OwnerDataEraseStorageResult::PhysicalResetRequired;
    }
    const uint64_t previous_sequence = newest < 0
                                           ? 0
                                           : candidates[static_cast<size_t>(newest)].sequence;
    if (err == ESP_OK && previous_sequence ==
                             std::numeric_limits<uint64_t>::max()) {
        err = ESP_ERR_INVALID_STATE;
    }
    const uint64_t sequence = previous_sequence + 1;
    std::vector<uint8_t> blob;
    if (err == ESP_OK && !EncodeProgress(value, sequence, blob)) {
        err = ESP_ERR_INVALID_ARG;
    }
    const char* slot = kProgressSlots[sequence & 1U];
    if (err == ESP_OK) err = nvs_set_blob(handle, slot, blob.data(), blob.size());
    if (err == ESP_OK) err = nvs_commit(handle);
    std::vector<uint8_t> readback;
    bool exists = false;
    if (err == ESP_OK) err = ReadBlob(handle, slot, readback, exists);
    if (err == ESP_OK && (!exists || readback != blob)) err = ESP_FAIL;
    nvs_close(handle);
    return Classify(err);
}

OwnerDataEraseStorageResult EspIdfOwnerDataEraseStorage::EraseTarget(
    const OwnerDataEraseTarget& target) {
    if (target.kind == OwnerDataEraseTargetKind::NvsKeys) {
        nvs_handle_t handle = 0;
        esp_err_t err = OpenTarget(target, NVS_READWRITE, handle);
        if (err != ESP_OK) return target.optional && Classify(err) ==
                                           OwnerDataEraseStorageResult::NotFound
                                       ? OwnerDataEraseStorageResult::NotFound
                                       : Classify(err);
        for (const auto& key : target.keys) {
            err = nvs_erase_key(handle, key.c_str());
            if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
            if (err != ESP_OK) break;
        }
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
        return Classify(err);
    }

    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
        target.partition_label.c_str());
    if (partition == nullptr) {
        return target.optional ? OwnerDataEraseStorageResult::NotFound
                               : OwnerDataEraseStorageResult::PhysicalResetRequired;
    }
    return Classify(esp_partition_erase_range(partition, 0, partition->size));
}

OwnerDataEraseStorageResult EspIdfOwnerDataEraseStorage::VerifyTargetErased(
    const OwnerDataEraseTarget& target) {
    if (target.kind == OwnerDataEraseTargetKind::NvsKeys) {
        nvs_iterator_t iterator = nullptr;
        const esp_err_t err = FirstTargetEntry(target, iterator);
        if (iterator != nullptr) nvs_release_iterator(iterator);
        if (err == ESP_OK) return OwnerDataEraseStorageResult::RetryableFailure;
        return Classify(err);
    }

    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
        target.partition_label.c_str());
    if (partition == nullptr) {
        return target.optional ? OwnerDataEraseStorageResult::NotFound
                               : OwnerDataEraseStorageResult::PhysicalResetRequired;
    }
    std::array<uint8_t, 256> bytes{};
    for (size_t offset = 0; offset < partition->size; offset += bytes.size()) {
        const size_t length = std::min<size_t>(
            bytes.size(), static_cast<size_t>(partition->size) - offset);
        const esp_err_t err = esp_partition_read(partition, offset,
                                                 bytes.data(), length);
        if (err != ESP_OK) return Classify(err);
        if (!std::all_of(bytes.begin(), bytes.begin() + length,
                         [](uint8_t byte) { return byte == 0xff; })) {
            return OwnerDataEraseStorageResult::RetryableFailure;
        }
    }
    return OwnerDataEraseStorageResult::Done;
}

DeviceEraseJournalLoadResult EspIdfDeviceEraseJournal::Load(
    DeviceEraseJournalEntry& out) {
    out = {};
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kDeviceEraseJournalNvsNamespace,
                             NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return DeviceEraseJournalLoadResult::NotFound;
    if (err != ESP_OK) return DeviceEraseJournalLoadResult::StorageFailure;
    std::array<StoredBlob, 2> candidates;
    std::array<DeviceEraseJournalEntry, 2> decoded;
    err = ReadSlots(handle, kCoreJournalSlots, DecodeCoreJournal,
                    candidates, decoded);
    nvs_close(handle);
    if (err != ESP_OK) {
        return DeviceEraseJournalLoadResult::StorageFailure;
    }
    const int newest = NewestValidSlot(candidates);
    if (newest < 0) {
        if (!AnySlotExists(candidates)) return DeviceEraseJournalLoadResult::NotFound;
        return DeviceEraseJournalLoadResult::StorageFailure;
    }
    out = std::move(decoded[static_cast<size_t>(newest)]);
    return DeviceEraseJournalLoadResult::Loaded;
}

bool EspIdfDeviceEraseJournal::Store(const DeviceEraseJournalEntry& value) {
    if (!ValidCoreJournal(value)) return false;
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kDeviceEraseJournalNvsNamespace,
                             NVS_READWRITE, &handle);
    if (err != ESP_OK) return false;
    std::array<StoredBlob, 2> candidates;
    std::array<DeviceEraseJournalEntry, 2> decoded;
    err = ReadSlots(handle, kCoreJournalSlots, DecodeCoreJournal,
                    candidates, decoded);
    const int newest = err == ESP_OK ? NewestValidSlot(candidates) : -1;
    if (err == ESP_OK && newest < 0 && AnySlotExists(candidates)) err = ESP_FAIL;
    const uint64_t previous_sequence = newest < 0
                                           ? 0
                                           : candidates[static_cast<size_t>(newest)].sequence;
    if (err == ESP_OK && previous_sequence ==
                             std::numeric_limits<uint64_t>::max()) {
        err = ESP_ERR_INVALID_STATE;
    }
    const uint64_t sequence = previous_sequence + 1;
    std::vector<uint8_t> blob;
    if (err == ESP_OK && !EncodeCoreJournal(value, sequence, blob)) {
        err = ESP_ERR_INVALID_ARG;
    }
    const char* slot = kCoreJournalSlots[sequence & 1U];
    if (err == ESP_OK) err = nvs_set_blob(handle, slot, blob.data(), blob.size());
    if (err == ESP_OK) err = nvs_commit(handle);
    std::vector<uint8_t> readback;
    bool exists = false;
    if (err == ESP_OK) err = ReadBlob(handle, slot, readback, exists);
    if (err == ESP_OK && (!exists || readback != blob)) err = ESP_FAIL;
    nvs_close(handle);
    return err == ESP_OK;
}

}  // namespace eidolon
