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

}  // namespace

int main() {
    OrdinaryBootCannotConsumeEraseTerminal();
    PhysicalPresenceRotatesIdentityAndArchivesEvidence();
    EveryCommittedPhaseResumesAfterCrash();
    OldIdentityOrOperationCannotReuseArchive();
}
