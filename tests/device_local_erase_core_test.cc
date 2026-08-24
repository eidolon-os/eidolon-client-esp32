#include "eidolon/device_local_erase_core.h"

#include <cassert>
#include <string>

using eidolon::DeviceEraseAckSignerPort;
using eidolon::DeviceEraseAdapterOutcome;
using eidolon::DeviceEraseAdapterResult;
using eidolon::DeviceEraseClockPort;
using eidolon::DeviceEraseCoreResult;
using eidolon::DeviceEraseJournalEntry;
using eidolon::DeviceEraseJournalLoadResult;
using eidolon::DeviceEraseJournalPhase;
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
        1,
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
    DeviceEraseJournalLoadResult Load(DeviceEraseJournalEntry& out) override {
        if (fail_load) return DeviceEraseJournalLoadResult::StorageFailure;
        if (!present) return DeviceEraseJournalLoadResult::NotFound;
        out = value;
        return DeviceEraseJournalLoadResult::Loaded;
    }
    bool Store(const DeviceEraseJournalEntry& next) override {
        ++store_attempts;
        if (fail_store || fail_store_call == store_attempts) return false;
        value = next;
        present = true;
        ++stores;
        return true;
    }

    bool present = false;
    bool fail_load = false;
    bool fail_store = false;
    int fail_store_call = -1;
    int store_attempts = 0;
    int stores = 0;
    DeviceEraseJournalEntry value;
};

class Adapter final : public DeviceLocalEraseAdapterPort {
public:
    DeviceEraseAdapterOutcome PrepareOwnerState(
        const DeviceLocalEraseCommand&) override {
        ++prepare_calls;
        return prepare_outcome;
    }
    DeviceEraseAdapterOutcome FinalizeOwnerState(
        const DeviceLocalEraseCommand&) override {
        ++finalize_calls;
        return finalize_outcome;
    }

    int prepare_calls = 0;
    int finalize_calls = 0;
    DeviceEraseAdapterOutcome prepare_outcome{
        DeviceEraseAdapterResult::PreparedForFinalization, "PREPARED"};
    DeviceEraseAdapterOutcome finalize_outcome{
        DeviceEraseAdapterResult::Erased, "ERASED"};
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
    assert(adapter.prepare_calls == 1);
    assert(adapter.finalize_calls == 1);
    assert(journal.stores == 5);
    assert(journal.value.phase == DeviceEraseJournalPhase::DurableTerminal);
    assert(signer.last_document ==
           "{\"ack_sequence\":1,\"contract\":\"eidolon.device-foundation.device-operation-ack\",\"contract_version\":\"1.0\",\"device_monotonic_time\":1234,\"device_ref\":{\"accepted_manifest_digest\":\"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"claim_generation\":7,\"device_instance_id\":\"device_erase_01\",\"owner_domain_generation\":1,\"owner_domain_id\":\"owner_01\",\"trust_epoch\":4},\"operation_id\":\"erase_operation_01\",\"operation_type\":\"device-local.erase\",\"result\":\"erased\",\"result_code\":\"ERASED\"}");
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
    assert(adapter.prepare_calls == 1);
    assert(adapter.finalize_calls == 1);
    assert(signer.calls == 1);
}

void SameIdDifferentPayloadIsAnIdempotencyConflict() {
    Journal journal;
    Adapter adapter;
    Clock clock;
    Signer signer;
    DeviceLocalEraseCore core(Ref(), journal, adapter, clock, signer);
    const std::string fingerprint = "sha256:" + std::string(64, '1');
    assert(core.Handle(Command(), fingerprint).result ==
           DeviceEraseCoreResult::Acknowledged);
    auto changed = Command();
    changed.erase_scopes = {"owner-data"};
    assert(core.Handle(changed, "sha256:" + std::string(64, '2')).result ==
           DeviceEraseCoreResult::IdempotencyConflict);
    assert(core.Handle(changed, fingerprint).result ==
           DeviceEraseCoreResult::IdempotencyConflict);
    assert(adapter.prepare_calls == 1);
}

void LoadedTerminalWithoutAckEvidenceFailsClosed() {
    Journal journal;
    journal.present = true;
    journal.value.operation_id = Command().operation_id;
    journal.value.request_fingerprint = "sha256:" + std::string(64, 'a');
    journal.value.device_ref = Ref();
    journal.value.deadline = Command().deadline;
    journal.value.erase_scopes = Command().erase_scopes;
    journal.value.phase = DeviceEraseJournalPhase::DurableTerminal;
    Adapter adapter;
    Clock clock;
    Signer signer;
    DeviceLocalEraseCore core(Ref(), journal, adapter, clock, signer);
    assert(core.Handle(Command(), journal.value.request_fingerprint).result ==
           DeviceEraseCoreResult::StorageFailure);
    assert(adapter.prepare_calls == 0);
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
    assert(adapter.prepare_calls == 0);
    assert(adapter.finalize_calls == 0);
}

void PermanentFailureIsSignedAndReplayedAsTerminal() {
    Journal journal;
    Adapter adapter;
    adapter.prepare_outcome = {
        DeviceEraseAdapterResult::PhysicalResetRequired,
        "PHYSICAL_RESET_REQUIRED"};
    Clock clock;
    Signer signer;
    DeviceLocalEraseCore core(Ref(), journal, adapter, clock, signer);
    const std::string fingerprint = "sha256:" + std::string(64, '5');
    const auto outcome = core.Handle(Command(), fingerprint);
    assert(outcome.result == DeviceEraseCoreResult::Acknowledged);
    assert(outcome.ack.result == DeviceLocalEraseResult::PermanentFailure);
    assert(outcome.ack.result_code == "PHYSICAL_RESET_REQUIRED");
    assert(core.Handle(Command(), fingerprint).result ==
           DeviceEraseCoreResult::Replayed);
    assert(adapter.prepare_calls == 1);
    assert(adapter.finalize_calls == 0);
}

void RetryableStorageFailureDoesNotSignOrAck() {
    Journal journal;
    Adapter adapter;
    adapter.prepare_outcome = {
        DeviceEraseAdapterResult::RetryableStorageFailure,
        "RETRYABLE_STORAGE_FAILURE"};
    Clock clock;
    Signer signer;
    DeviceLocalEraseCore core(Ref(), journal, adapter, clock, signer);
    const std::string fingerprint = "sha256:" + std::string(64, '7');

    const auto failed = core.Handle(Command(), fingerprint);
    assert(failed.result == DeviceEraseCoreResult::RetryableStorageFailure);
    assert(!failed.has_ack);
    assert(signer.calls == 0);
    assert(journal.value.phase == DeviceEraseJournalPhase::Erasing);

    adapter.prepare_outcome = {
        DeviceEraseAdapterResult::PreparedForFinalization, "PREPARED"};
    const auto retried = core.Handle(Command(), fingerprint);
    assert(retried.result == DeviceEraseCoreResult::Acknowledged);
    assert(retried.has_ack);
    assert(adapter.prepare_calls == 2);
    assert(adapter.finalize_calls == 1);
}

void CorruptOrUnreadableJournalFailsClosedBeforeErase() {
    Journal journal;
    journal.fail_load = true;
    Adapter adapter;
    Clock clock;
    Signer signer;
    DeviceLocalEraseCore core(Ref(), journal, adapter, clock, signer);
    const auto outcome = core.Handle(
        Command(), "sha256:" + std::string(64, 'c'));
    assert(outcome.result == DeviceEraseCoreResult::StorageFailure);
    assert(adapter.prepare_calls == 0);
    assert(signer.calls == 0);
}

void DestructiveResumeCrossesDeadlineForSameOperation() {
    Journal journal;
    Adapter adapter;
    adapter.finalize_outcome = {
        DeviceEraseAdapterResult::RetryableStorageFailure,
        "RETRYABLE_STORAGE_FAILURE"};
    Clock clock;
    Signer signer;
    DeviceLocalEraseCore first(Ref(), journal, adapter, clock, signer);
    const std::string fingerprint = "sha256:" + std::string(64, '8');
    assert(first.Handle(Command(), fingerprint).result ==
           DeviceEraseCoreResult::RetryableStorageFailure);
    assert(journal.value.phase == DeviceEraseJournalPhase::Erasing);
    assert(journal.value.has_staged_ack);

    clock.expired = true;
    adapter.finalize_outcome = {DeviceEraseAdapterResult::Erased, "ERASED"};
    DeviceLocalEraseCore restarted(Ref(), journal, adapter, clock, signer);
    const auto resumed = restarted.Handle(Command(), fingerprint);
    assert(resumed.result == DeviceEraseCoreResult::Acknowledged);
    assert(resumed.has_ack);
    assert(adapter.prepare_calls == 1);
    assert(adapter.finalize_calls == 2);
}

void BootResumeUsesDurableCommandWithoutRedelivery() {
    Journal journal;
    Adapter adapter;
    adapter.finalize_outcome = {
        DeviceEraseAdapterResult::RetryableStorageFailure,
        "RETRYABLE_STORAGE_FAILURE"};
    Clock clock;
    Signer signer;
    const std::string fingerprint = "sha256:" + std::string(64, 'e');
    DeviceLocalEraseCore first(Ref(), journal, adapter, clock, signer);
    assert(first.Handle(Command(), fingerprint).result ==
           DeviceEraseCoreResult::RetryableStorageFailure);

    clock.expired = true;
    adapter.finalize_outcome = {DeviceEraseAdapterResult::Erased, "ERASED"};
    DeviceLocalEraseCore rebooted(Ref(), journal, adapter, clock, signer);
    const auto resumed = rebooted.ResumePending();
    assert(resumed.result == DeviceEraseCoreResult::Acknowledged);
    assert(resumed.has_ack);
    assert(adapter.prepare_calls == 1);
    assert(adapter.finalize_calls == 2);
}

void StagedAckIsDurableBeforeOperationalCredentialFinalization() {
    Journal journal;
    Adapter adapter;
    Clock clock;
    Signer signer;
    DeviceLocalEraseCore core(Ref(), journal, adapter, clock, signer);
    const std::string fingerprint = "sha256:" + std::string(64, '9');
    adapter.finalize_outcome = {
        DeviceEraseAdapterResult::RetryableStorageFailure,
        "RETRYABLE_STORAGE_FAILURE"};

    assert(core.Handle(Command(), fingerprint).result ==
           DeviceEraseCoreResult::RetryableStorageFailure);
    assert(journal.value.phase == DeviceEraseJournalPhase::Erasing);
    assert(journal.value.has_staged_ack);
    assert(!journal.value.staged_ack.device_signature.empty());
    assert(adapter.finalize_calls == 1);
}

void NewClaimCannotResumeOldDestructiveOperation() {
    Journal journal;
    Adapter adapter;
    adapter.prepare_outcome = {
        DeviceEraseAdapterResult::RetryableStorageFailure,
        "RETRYABLE_STORAGE_FAILURE"};
    Clock clock;
    Signer signer;
    const std::string fingerprint = "sha256:" + std::string(64, 'a');
    DeviceLocalEraseCore old_core(Ref(7), journal, adapter, clock, signer);
    assert(old_core.Handle(Command(7), fingerprint).result ==
           DeviceEraseCoreResult::RetryableStorageFailure);

    DeviceLocalEraseCore new_core(Ref(8), journal, adapter, clock, signer);
    assert(new_core.Handle(Command(7), fingerprint).result ==
           DeviceEraseCoreResult::StaleGeneration);
    assert(adapter.prepare_calls == 1);
}

void EveryCoreJournalCheckpointFailsClosedAndResumes() {
    for (int failure = 1; failure <= 5; ++failure) {
        Journal journal;
        journal.fail_store_call = failure;
        Adapter adapter;
        Clock clock;
        Signer signer;
        DeviceLocalEraseCore core(Ref(), journal, adapter, clock, signer);
        const std::string fingerprint =
            "sha256:" + std::string(63, 'd') + std::to_string(failure);
        const auto interrupted = core.Handle(Command(), fingerprint);
        assert(interrupted.result == DeviceEraseCoreResult::StorageFailure);
        assert(!interrupted.has_ack);

        journal.fail_store_call = -1;
        DeviceLocalEraseCore restarted(Ref(), journal, adapter, clock, signer);
        const auto resumed = restarted.Handle(Command(), fingerprint);
        assert(resumed.result == DeviceEraseCoreResult::Acknowledged);
        assert(resumed.has_ack);
        assert(journal.value.phase == DeviceEraseJournalPhase::DurableTerminal);
    }
}

void NewClaimCanUseFreshOperationAfterPriorDurableTerminal() {
    Journal journal;
    Adapter adapter;
    Clock clock;
    Signer signer;
    DeviceLocalEraseCore old_core(Ref(7), journal, adapter, clock, signer);
    auto old_command = Command(7);
    assert(old_core.Handle(old_command, "sha256:" + std::string(64, 'f')).result ==
           DeviceEraseCoreResult::Acknowledged);

    auto new_command = Command(8);
    new_command.operation_id = "erase_operation_02";
    DeviceLocalEraseCore new_core(Ref(8), journal, adapter, clock, signer);
    const auto next = new_core.Handle(
        new_command, "sha256:" + std::string(64, '0'));
    assert(next.result == DeviceEraseCoreResult::Acknowledged);
    assert(next.has_ack);
    assert(next.ack.operation_id == "erase_operation_02");
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
    LoadedTerminalWithoutAckEvidenceFailsClosed();
    DeadlineAndOldGenerationNeverReachTheAdapter();
    PermanentFailureIsSignedAndReplayedAsTerminal();
    RetryableStorageFailureDoesNotSignOrAck();
    CorruptOrUnreadableJournalFailsClosedBeforeErase();
    DestructiveResumeCrossesDeadlineForSameOperation();
    BootResumeUsesDurableCommandWithoutRedelivery();
    StagedAckIsDurableBeforeOperationalCredentialFinalization();
    NewClaimCannotResumeOldDestructiveOperation();
    EveryCoreJournalCheckpointFailsClosedAndResumes();
    NewClaimCanUseFreshOperationAfterPriorDurableTerminal();
    HostRelocationDoesNotEnterCoreIdentityOrOperationId();
    return 0;
}
