#ifndef EIDOLON_DEVICE_BOOT_RECOVERY_H_
#define EIDOLON_DEVICE_BOOT_RECOVERY_H_

#include "device_local_erase_core.h"

namespace eidolon {

class DeviceBootRecovery {
public:
    // Runs before Hub discovery, Claim collection, or operational runtime.
    // Only an absent RemovalJournal permits those later stages.
    static DeviceEraseCoreOutcome ResumePendingRemoval();
    static bool AllowsClaimOrRuntime(const DeviceEraseCoreOutcome& outcome) {
        return outcome.result == DeviceEraseCoreResult::NoPendingOperation;
    }
};

}  // namespace eidolon

#endif  // EIDOLON_DEVICE_BOOT_RECOVERY_H_
