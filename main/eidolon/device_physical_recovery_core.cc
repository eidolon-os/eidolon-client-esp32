#include "device_physical_recovery_core.h"

namespace eidolon {

bool RemovalJournalBlocksCommissioning(
    DeviceEraseJournalLoadResult loaded,
    const DeviceEraseJournalEntry& entry,
    bool terminal_consumed_by_physical_recovery) {
    switch (loaded) {
    case DeviceEraseJournalLoadResult::NotFound:
        // Nothing was ever removed from this device. Factory state, and the
        // state a device is in for its whole ordinary life.
        return false;
    case DeviceEraseJournalLoadResult::StorageFailure:
        return true;
    case DeviceEraseJournalLoadResult::Loaded:
        break;
    }
    // Before the terminal there is no evidence to consume: the erase is still
    // running, or stopped partway. Either way this device is mid-removal, and
    // mid-removal is not a device that may offer itself to a new Owner.
    if (entry.phase != DeviceEraseJournalPhase::DurableTerminal &&
        entry.phase != DeviceEraseJournalPhase::ArchivedTerminal) {
        return true;
    }
    return !terminal_consumed_by_physical_recovery;
}

bool DevicePhysicalRecoveryCore::ValidTerminal(
    const DeviceEraseJournalEntry& terminal) {
    return (terminal.phase == DeviceEraseJournalPhase::DurableTerminal ||
            terminal.phase == DeviceEraseJournalPhase::ArchivedTerminal) &&
           terminal.has_staged_ack && !terminal.operation_id.empty() &&
           terminal.staged_ack.operation_id == terminal.operation_id &&
           terminal.staged_ack.result ==
               device_foundation::v1::DeviceLocalEraseResult::Erased &&
           !terminal.device_ref.device_instance_id.empty() &&
           !terminal.staged_ack.device_signature.empty();
}

bool DevicePhysicalRecoveryCore::Matches(
    const PhysicalRecoveryRecord& record,
    const DeviceEraseJournalEntry& terminal) {
    return record.operation_id == terminal.operation_id &&
           record.old_device_instance_id ==
               terminal.device_ref.device_instance_id &&
           record.terminal_signature == terminal.staged_ack.device_signature;
}

PhysicalRecoveryResult DevicePhysicalRecoveryCore::AuthorizeAndRun(
    const DeviceEraseJournalEntry& terminal, bool physical_presence) {
    if (!ValidTerminal(terminal)) return PhysicalRecoveryResult::InvalidTerminal;
    PhysicalRecoveryRecord record;
    const auto loaded = journal_.Load(record);
    if (loaded == PhysicalRecoveryLoadResult::StorageFailure) {
        return PhysicalRecoveryResult::StorageFailure;
    }
    // A record that does not describe the terminal in hand is a record of a
    // different transaction — a previous removal this device already recovered
    // from — and it is not evidence about this one. It used to be read as one,
    // and the answer it gave was always no: every second removal on the same
    // hardware refused physical presence with InvalidTerminal, forever, because
    // the finished record's operation_id, old instance and terminal signature
    // can never match the new terminal's. There is exactly one slot, so the
    // stale entry is not merely unhelpful; it is the veto.
    //
    // Who may write over it is the whole question, and the two callers differ:
    // a person holding the button is the authority this design uses to start a
    // recovery, so they may start a fresh one over a finished one. A boot
    // resume may not — it carries no authorization of its own, and treating a
    // mismatch as permission to begin is how a reboot would mint the capability
    // the button is there to provide.
    const bool describes_this_terminal =
        loaded == PhysicalRecoveryLoadResult::Loaded && Matches(record, terminal);
    if (!describes_this_terminal) {
        if (!physical_presence) {
            return loaded == PhysicalRecoveryLoadResult::NotFound
                       ? PhysicalRecoveryResult::PhysicalPresenceRequired
                       : PhysicalRecoveryResult::InvalidTerminal;
        }
        record = PhysicalRecoveryRecord{};
        record.operation_id = terminal.operation_id;
        record.old_device_instance_id = terminal.device_ref.device_instance_id;
        record.terminal_signature = terminal.staged_ack.device_signature;
        record.phase = PhysicalRecoveryPhase::Authorized;
        if (!journal_.Store(record)) return PhysicalRecoveryResult::StorageFailure;
    }
    return Run(terminal, record);
}

PhysicalRecoveryResult DevicePhysicalRecoveryCore::Resume(
    const DeviceEraseJournalEntry& terminal) {
    return AuthorizeAndRun(terminal, false);
}

PhysicalRecoveryResult DevicePhysicalRecoveryCore::Run(
    const DeviceEraseJournalEntry& terminal, PhysicalRecoveryRecord record) {
    if (!Matches(record, terminal)) return PhysicalRecoveryResult::InvalidTerminal;
    if (record.phase == PhysicalRecoveryPhase::Authorized) {
        if (!identity_.ClearOldOwnerAndOperationalState()) {
            return PhysicalRecoveryResult::IdentityFailure;
        }
        record.phase = PhysicalRecoveryPhase::OldPrincipalCleared;
        if (!journal_.Store(record)) return PhysicalRecoveryResult::StorageFailure;
    }
    if (record.phase == PhysicalRecoveryPhase::OldPrincipalCleared) {
        std::string instance_id;
        if (!identity_.CreateFreshOperationalIdentity(instance_id) ||
            instance_id.empty() ||
            instance_id == record.old_device_instance_id) {
            return PhysicalRecoveryResult::IdentityFailure;
        }
        record.new_device_instance_id = instance_id;
        record.phase = PhysicalRecoveryPhase::NewPrincipalCreated;
        if (!journal_.Store(record)) return PhysicalRecoveryResult::StorageFailure;
    }
    if (record.phase == PhysicalRecoveryPhase::NewPrincipalCreated) {
        if (record.new_device_instance_id.empty() ||
            record.new_device_instance_id == record.old_device_instance_id) {
            return PhysicalRecoveryResult::IdentityFailure;
        }
        record.phase = PhysicalRecoveryPhase::Commissionable;
        if (!journal_.Store(record)) return PhysicalRecoveryResult::StorageFailure;
    }
    return record.phase == PhysicalRecoveryPhase::Commissionable
               ? PhysicalRecoveryResult::Commissionable
               : PhysicalRecoveryResult::StorageFailure;
}

bool DevicePhysicalRecoveryCore::ConsumedTerminal(
    const DeviceEraseJournalEntry& terminal) const {
    PhysicalRecoveryRecord record;
    return ValidTerminal(terminal) &&
           journal_.Load(record) == PhysicalRecoveryLoadResult::Loaded &&
           Matches(record, terminal) &&
           record.phase == PhysicalRecoveryPhase::Commissionable &&
           !record.new_device_instance_id.empty() &&
           record.new_device_instance_id != record.old_device_instance_id;
}

}  // namespace eidolon
