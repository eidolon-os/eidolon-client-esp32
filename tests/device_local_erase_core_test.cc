#include "eidolon/device_local_erase_core.h"

#include <cassert>
#include <string>

using eidolon::DeviceEraseAckSignerPort;
using eidolon::DeviceEraseAdapterOutcome;
using eidolon::DeviceEraseAdapterResult;
using eidolon::DeviceEraseClockPort;
using eidolon::DeviceEraseCoreResult;
using eidolon::DeviceEraseJournalEntry;
using eidolon::DeviceEraseJournalPort;
using eidolon::DeviceLocalEraseAdapterPort;
using eidolon::DeviceLocalEraseCore;
using eidolon::device_foundation::v1::DeviceLocalEraseCommand;
using eidolon::device_foundation::v1::DeviceLocalEraseResult;
using eidolon::device_foundation::v1::DeviceRef;

namespace {

DeviceRef Ref(uint32_t generation = 7) {
    return DeviceRef{
        "device_erase_01",
        "owner_01",
        generation,
        4,
        "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    };
}

DeviceLocalEraseCommand Command(uint32_t generation = 7) {
    return DeviceLocalEraseCommand{
        "erase_operation_01",
        Ref(generation),
        "2026-08-30T00:00:00Z",
        {"owner-credentials", "owner-data", "network-profiles"},
    };
}

class Journal final : public DeviceEraseJournalPort {
public:
    bool Load(DeviceEraseJournalEntry& out) override {
        if (!present) return false;
        out = value;
        return true;
    }
    bool Store(const DeviceEraseJournalEntry& next) override {
        if (fail_store) return false;
        value = next;
        present = true;
        ++stores;
        return true;
    }

    bool present = false;
    bool fail_store = false;
    int stores = 0;
    DeviceEraseJournalEntry value;
};

class Adapter final : public DeviceLocalEraseAdapterPort {
public:
    DeviceEraseAdapterOutcome EraseOwnerState(
        const DeviceLocalEraseCommand&) override {
        ++calls;
        return outcome;
    }

    int calls = 0;
    DeviceEraseAdapterOutcome outcome{DeviceEraseAdapterResult::Erased, "ERASED"};
};

class Clock final : public DeviceEraseClockPort {
public:
    bool DeadlineExpired(const std::string&) const override { return expired; }
    uint64_t MonotonicTime() const override { return monotonic; }

    bool expired = false;
    uint64_t monotonic = 1234;
};

class Signer final : public DeviceEraseAckSignerPort {
public:
    bool SignCanonical(const std::string& canonical,
                       std::string& signature) override {
        ++calls;
        last_document = canonical;
        if (fail) return false;
        signature = std::string(86, 'A');
        return true;
    }

    bool fail = false;
    int calls = 0;
    std::string last_document;
};

void OnlineAckUsesCanonicalGenerationBoundDocument() {
    Journal journal;
    Adapter adapter;
    Clock clock;
    Signer signer;
    DeviceLocalEraseCore core(Ref(), journal, adapter, clock, signer);

    const auto outcome = core.Handle(
        Command(), "sha256:a1de19e602f74a44a5b4c8a9d894fb46ab669f1e662be3ba2c85669b21f62060");
    assert(outcome.result == DeviceEraseCoreResult::Acknowledged);
    assert(outcome.has_ack);
    assert(outcome.ack.result == DeviceLocalEraseResult::Erased);
    assert(adapter.calls == 1);
    assert(journal.stores == 2);
    assert(signer.last_document ==
           "{\"ack_sequence\":1,\"contract\":\"eidolon.device-foundation.device-operation-ack\",\"contract_version\":\"1.0\",\"device_monotonic_time\":1234,\"device_ref\":{\"accepted_manifest_digest\":\"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"claim_generation\":7,\"device_instance_id\":\"device_erase_01\",\"owner_domain_id\":\"owner_01\",\"trust_epoch\":4},\"operation_id\":\"erase_operation_01\",\"operation_type\":\"device-local.erase\",\"result\":\"erased\",\"result_code\":\"ERASED\"}");
}

void DuplicateAndRestartReplayOneAckWithoutSecondErase() {
    Journal journal;
    Adapter adapter;
    Clock clock;
    Signer signer;
    const std::string fingerprint = "sha256:" + std::string(64, 'b');
    {
        DeviceLocalEraseCore first(Ref(), journal, adapter, clock, signer);
        assert(first.Handle(Command(), fingerprint).result ==
               DeviceEraseCoreResult::Acknowledged);
    }
    DeviceLocalEraseCore restarted(Ref(), journal, adapter, clock, signer);
    const auto replay = restarted.Handle(Command(), fingerprint);
    assert(replay.result == DeviceEraseCoreResult::Replayed);
    assert(replay.has_ack);
    assert(adapter.calls == 1);
    assert(signer.calls == 1);
}

void SameIdDifferentPayloadIsAnIdempotencyConflict() {
    Journal journal;
    Adapter adapter;
    Clock clock;
    Signer signer;
    DeviceLocalEraseCore core(Ref(), journal, adapter, clock, signer);
    assert(core.Handle(Command(), "sha256:" + std::string(64, '1')).result ==
           DeviceEraseCoreResult::Acknowledged);
    auto changed = Command();
    changed.erase_scopes = {"owner-data"};
    assert(core.Handle(changed, "sha256:" + std::string(64, '2')).result ==
           DeviceEraseCoreResult::IdempotencyConflict);
    assert(adapter.calls == 1);
}

void DeadlineAndOldGenerationNeverReachTheAdapter() {
    Journal journal;
    Adapter adapter;
    Clock clock;
    Signer signer;
    DeviceLocalEraseCore core(Ref(8), journal, adapter, clock, signer);
    assert(core.Handle(Command(7), "sha256:" + std::string(64, '3')).result ==
           DeviceEraseCoreResult::StaleGeneration);
    clock.expired = true;
    assert(core.Handle(Command(8), "sha256:" + std::string(64, '4')).result ==
           DeviceEraseCoreResult::Expired);
    assert(adapter.calls == 0);
}

void PermanentFailureIsSignedAndReplayedAsTerminal() {
    Journal journal;
    Adapter adapter;
    adapter.outcome = {
        DeviceEraseAdapterResult::PermanentFailure, "LOCAL_STORE_DAMAGED"};
    Clock clock;
    Signer signer;
    DeviceLocalEraseCore core(Ref(), journal, adapter, clock, signer);
    const std::string fingerprint = "sha256:" + std::string(64, '5');
    const auto outcome = core.Handle(Command(), fingerprint);
    assert(outcome.result == DeviceEraseCoreResult::Acknowledged);
    assert(outcome.ack.result == DeviceLocalEraseResult::PermanentFailure);
    assert(outcome.ack.result_code == "LOCAL_STORE_DAMAGED");
    assert(core.Handle(Command(), fingerprint).result ==
           DeviceEraseCoreResult::Replayed);
    assert(adapter.calls == 1);
}

void HostRelocationDoesNotEnterCoreIdentityOrOperationId() {
    Journal journal;
    Adapter adapter;
    Clock clock;
    Signer signer;
    DeviceLocalEraseCore before(Ref(), journal, adapter, clock, signer);
    const std::string fingerprint = "sha256:" + std::string(64, '6');
    const auto first = before.Handle(Command(), fingerprint);
    DeviceLocalEraseCore after(Ref(), journal, adapter, clock, signer);
    const auto relocated = after.Handle(Command(), fingerprint);
    assert(first.ack.operation_id == relocated.ack.operation_id);
    assert(relocated.result == DeviceEraseCoreResult::Replayed);
}

}  // namespace

int main() {
    OnlineAckUsesCanonicalGenerationBoundDocument();
    DuplicateAndRestartReplayOneAckWithoutSecondErase();
    SameIdDifferentPayloadIsAnIdempotencyConflict();
    DeadlineAndOldGenerationNeverReachTheAdapter();
    PermanentFailureIsSignedAndReplayedAsTerminal();
    HostRelocationDoesNotEnterCoreIdentityOrOperationId();
    return 0;
}
