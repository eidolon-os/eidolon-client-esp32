#include "eidolon/device_physical_recovery_core.h"

#include <cassert>
#include <string>

using namespace eidolon;

namespace {

DeviceEraseJournalEntry Terminal() {
    DeviceEraseJournalEntry value;
    value.operation_id = "erase-old-1";
    value.device_ref.device_instance_id = "device-instance-old";
    value.phase = DeviceEraseJournalPhase::DurableTerminal;
    value.has_staged_ack = true;
    value.staged_ack.operation_id = value.operation_id;
    value.staged_ack.device_ref = value.device_ref;
    value.staged_ack.result =
        device_foundation::v1::DeviceLocalEraseResult::Erased;
    value.staged_ack.device_signature = "signed-terminal-evidence";
    return value;
}

class Journal final : public PhysicalRecoveryJournalPort {
public:
    PhysicalRecoveryLoadResult Load(PhysicalRecoveryRecord& out) override {
        if (load_failure) return PhysicalRecoveryLoadResult::StorageFailure;
        if (!present) return PhysicalRecoveryLoadResult::NotFound;
        out = value;
        return PhysicalRecoveryLoadResult::Loaded;
    }
    bool Store(const PhysicalRecoveryRecord& next) override {
        ++stores;
        if (fail_store_at == stores) return false;
        value = next;
        present = true;
        return true;
    }
    bool present = false;
    bool load_failure = false;
    int stores = 0;
    int fail_store_at = 0;
    PhysicalRecoveryRecord value;
};

class Identity final : public PhysicalRecoveryIdentityPort {
public:
    bool ClearOldOwnerAndOperationalState() override {
        ++clears;
        return !fail_clear;
    }
    bool CreateFreshOperationalIdentity(std::string& instance_id) override {
        ++creates;
        if (fail_create) return false;
        instance_id = next;
        return true;
    }
    int clears = 0;
    int creates = 0;
    bool fail_clear = false;
    bool fail_create = false;
    std::string next = "device-instance-new";
};

void OrdinaryBootCannotConsumeEraseTerminal() {
    Journal journal;
    Identity identity;
    DevicePhysicalRecoveryCore core(journal, identity);
    assert(core.Resume(Terminal()) ==
           PhysicalRecoveryResult::PhysicalPresenceRequired);
    assert(!journal.present && identity.clears == 0 && identity.creates == 0);
}

void PhysicalPresenceRotatesIdentityAndArchivesEvidence() {
    Journal journal;
    Identity identity;
    DevicePhysicalRecoveryCore core(journal, identity);
    assert(core.AuthorizeAndRun(Terminal(), true) ==
           PhysicalRecoveryResult::Commissionable);
    assert(journal.value.operation_id == "erase-old-1");
    assert(journal.value.terminal_signature == "signed-terminal-evidence");
    assert(journal.value.old_device_instance_id == "device-instance-old");
    assert(journal.value.new_device_instance_id == "device-instance-new");
    assert(journal.value.phase == PhysicalRecoveryPhase::Commissionable);
    assert(core.ConsumedTerminal(Terminal()));
}

void EveryCommittedPhaseResumesAfterCrash() {
    for (int fail = 2; fail <= 4; ++fail) {
        Journal journal;
        journal.fail_store_at = fail;
        Identity identity;
        DevicePhysicalRecoveryCore first(journal, identity);
        assert(first.AuthorizeAndRun(Terminal(), true) ==
               PhysicalRecoveryResult::StorageFailure);
        journal.fail_store_at = 0;
        DevicePhysicalRecoveryCore rebooted(journal, identity);
        assert(rebooted.Resume(Terminal()) ==
               PhysicalRecoveryResult::Commissionable);
        assert(rebooted.ConsumedTerminal(Terminal()));
    }
}

void OldIdentityOrOperationCannotReuseArchive() {
    Journal journal;
    Identity identity;
    DevicePhysicalRecoveryCore core(journal, identity);
    assert(core.AuthorizeAndRun(Terminal(), true) ==
           PhysicalRecoveryResult::Commissionable);
    auto other = Terminal();
    other.operation_id = "erase-forged";
    other.staged_ack.operation_id = other.operation_id;
    assert(core.Resume(other) == PhysicalRecoveryResult::InvalidTerminal);
    other = Terminal();
    other.device_ref.device_instance_id = "device-instance-new";
    other.staged_ack.device_ref = other.device_ref;
    assert(core.Resume(other) == PhysicalRecoveryResult::InvalidTerminal);
}

// The advertising decision. D8: a device may not open its own commissioning
// window; §1 item 10: only physical presence or an authenticated admin may.
// These pin the fact that decision consults, which is not the trust store.

void NothingRemovedDoesNotBlockSetup() {
    DeviceEraseJournalEntry none;
    assert(!RemovalJournalBlocksCommissioning(
        DeviceEraseJournalLoadResult::NotFound, none, false));
}

void AnUnconsumedRemovalBlocksSetup() {
    // The measured failure: the erase is finished and acknowledged, the trust
    // store is empty, and the Claim path still refuses. No window here.
    assert(RemovalJournalBlocksCommissioning(
        DeviceEraseJournalLoadResult::Loaded, Terminal(), false));
    auto archived = Terminal();
    archived.phase = DeviceEraseJournalPhase::ArchivedTerminal;
    assert(RemovalJournalBlocksCommissioning(
        DeviceEraseJournalLoadResult::Loaded, archived, false));
}

void AHalfFinishedRemovalBlocksSetup() {
    for (auto phase : {DeviceEraseJournalPhase::Accepted,
                       DeviceEraseJournalPhase::Staging,
                       DeviceEraseJournalPhase::Erasing}) {
        auto entry = Terminal();
        entry.phase = phase;
        // There is no terminal to consume yet, so no gesture can have consumed
        // one. The second argument being true must not change the answer.
        assert(RemovalJournalBlocksCommissioning(
            DeviceEraseJournalLoadResult::Loaded, entry, false));
        assert(RemovalJournalBlocksCommissioning(
            DeviceEraseJournalLoadResult::Loaded, entry, true));
    }
}

void AnUnreadableJournalBlocksSetup() {
    DeviceEraseJournalEntry unread;
    assert(RemovalJournalBlocksCommissioning(
        DeviceEraseJournalLoadResult::StorageFailure, unread, false));
    assert(RemovalJournalBlocksCommissioning(
        DeviceEraseJournalLoadResult::StorageFailure, Terminal(), true));
}

void PhysicalPresenceIsWhatUnblocksSetup() {
    // Same device, same journal, one long press apart: the gesture runs the
    // recovery transaction, and only then may this device advertise.
    Journal journal;
    Identity identity;
    DevicePhysicalRecoveryCore core(journal, identity);
    assert(RemovalJournalBlocksCommissioning(
        DeviceEraseJournalLoadResult::Loaded, Terminal(),
        core.ConsumedTerminal(Terminal())));
    assert(core.AuthorizeAndRun(Terminal(), true) ==
           PhysicalRecoveryResult::Commissionable);
    assert(!RemovalJournalBlocksCommissioning(
        DeviceEraseJournalLoadResult::Loaded, Terminal(),
        core.ConsumedTerminal(Terminal())));
}

// The second removal on the same hardware. Measured 2026-09-02: the box-3 had
// already been recovered once (erase_db6d…, 06480c8b -> 191468d4), and every
// long press after its second erase (erase_be053c…, ACKed 14:05:59) returned
// InvalidTerminal — the finished record from the first recovery was still the
// only thing in the slot, and it can never match a newer terminal.

DeviceEraseJournalEntry SecondTerminal() {
    DeviceEraseJournalEntry value = Terminal();
    value.operation_id = "erase-new-2";
    value.device_ref.device_instance_id = "device-instance-second";
    value.staged_ack.operation_id = value.operation_id;
    value.staged_ack.device_ref = value.device_ref;
    value.staged_ack.device_signature = "signed-second-terminal";
    return value;
}

void ASecondRemovalIsStillRecoverable() {
    Journal journal;
    Identity identity;
    DevicePhysicalRecoveryCore core(journal, identity);
    assert(core.AuthorizeAndRun(Terminal(), true) ==
           PhysicalRecoveryResult::Commissionable);

    // The first recovery minted this device's current identity; the second
    // erase is a terminal for THAT identity, so nothing about it matches the
    // record left behind.
    identity.next = "device-instance-third";
    assert(core.AuthorizeAndRun(SecondTerminal(), true) ==
           PhysicalRecoveryResult::Commissionable);
    assert(core.ConsumedTerminal(SecondTerminal()));
    // And the finished first record is gone rather than lingering to veto a
    // third: it described a transaction that is over.
    assert(!core.ConsumedTerminal(Terminal()));
}

void ARebootStillCannotStartOneOnItsOwn() {
    // The mismatch is permission to write only for the button. A resume that
    // treated it as permission would let a reboot mint the very capability
    // physical presence exists to provide.
    Journal journal;
    Identity identity;
    DevicePhysicalRecoveryCore core(journal, identity);
    assert(core.AuthorizeAndRun(Terminal(), true) ==
           PhysicalRecoveryResult::Commissionable);
    const int stores = journal.stores;
    assert(core.Resume(SecondTerminal()) ==
           PhysicalRecoveryResult::InvalidTerminal);
    assert(journal.stores == stores);
    assert(!core.ConsumedTerminal(SecondTerminal()));
}

}  // namespace

int main() {
    OrdinaryBootCannotConsumeEraseTerminal();
    PhysicalPresenceRotatesIdentityAndArchivesEvidence();
    EveryCommittedPhaseResumesAfterCrash();
    OldIdentityOrOperationCannotReuseArchive();
    NothingRemovedDoesNotBlockSetup();
    AnUnconsumedRemovalBlocksSetup();
    AHalfFinishedRemovalBlocksSetup();
    AnUnreadableJournalBlocksSetup();
    PhysicalPresenceIsWhatUnblocksSetup();
    ASecondRemovalIsStillRecoverable();
    ARebootStillCannotStartOneOnItsOwn();
}
