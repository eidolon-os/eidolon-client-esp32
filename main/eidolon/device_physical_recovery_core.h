#ifndef EIDOLON_DEVICE_PHYSICAL_RECOVERY_CORE_H_
#define EIDOLON_DEVICE_PHYSICAL_RECOVERY_CORE_H_

#include <cstdint>
#include <string>

#include "device_local_erase_core.h"

namespace eidolon {

enum class PhysicalRecoveryPhase : uint8_t {
    Authorized = 1,
    OldPrincipalCleared = 2,
    NewPrincipalCreated = 3,
    Commissionable = 4,
};

struct PhysicalRecoveryRecord {
    std::string operation_id;
    std::string old_device_instance_id;
    std::string terminal_signature;
    std::string new_device_instance_id;
    PhysicalRecoveryPhase phase = PhysicalRecoveryPhase::Authorized;
};

enum class PhysicalRecoveryLoadResult { NotFound, Loaded, StorageFailure };

class PhysicalRecoveryJournalPort {
public:
    virtual ~PhysicalRecoveryJournalPort() = default;
    virtual PhysicalRecoveryLoadResult Load(PhysicalRecoveryRecord& out) = 0;
    virtual bool Store(const PhysicalRecoveryRecord& value) = 0;
};

class PhysicalRecoveryIdentityPort {
public:
    virtual ~PhysicalRecoveryIdentityPort() = default;
    virtual bool ClearOldOwnerAndOperationalState() = 0;
    virtual bool CreateFreshOperationalIdentity(std::string& instance_id) = 0;
};

enum class PhysicalRecoveryResult {
    Commissionable,
    PhysicalPresenceRequired,
    InvalidTerminal,
    StorageFailure,
    IdentityFailure,
};

// One deliberately narrow transaction. A durable Authorized record is the
// physical-presence capability; after that point reboot may only resume it
// forward. The RemovalJournal remains immutable evidence.
class DevicePhysicalRecoveryCore {
public:
    DevicePhysicalRecoveryCore(PhysicalRecoveryJournalPort& journal,
                               PhysicalRecoveryIdentityPort& identity)
        : journal_(journal), identity_(identity) {}

    PhysicalRecoveryResult AuthorizeAndRun(
        const DeviceEraseJournalEntry& terminal, bool physical_presence);
    PhysicalRecoveryResult Resume(
        const DeviceEraseJournalEntry& terminal);
    bool ConsumedTerminal(const DeviceEraseJournalEntry& terminal) const;

private:
    PhysicalRecoveryResult Run(const DeviceEraseJournalEntry& terminal,
                               PhysicalRecoveryRecord record);
    static bool ValidTerminal(const DeviceEraseJournalEntry& terminal);
    static bool Matches(const PhysicalRecoveryRecord& record,
                        const DeviceEraseJournalEntry& terminal);

    PhysicalRecoveryJournalPort& journal_;
    PhysicalRecoveryIdentityPort& identity_;
};

}  // namespace eidolon

#endif
