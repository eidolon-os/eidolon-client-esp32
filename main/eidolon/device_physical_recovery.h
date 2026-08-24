#ifndef EIDOLON_DEVICE_PHYSICAL_RECOVERY_H_
#define EIDOLON_DEVICE_PHYSICAL_RECOVERY_H_

#include "device_local_erase_core.h"

namespace eidolon {

class DevicePhysicalRecovery {
public:
    // Called only from the explicit long-press path. If there is no removal
    // terminal it is a no-op; otherwise it durably authorizes and runs rejoin.
    static bool AuthorizeFromPhysicalPresence();
    // Boot may resume a transaction only after its authorization was durable.
    static bool ResumeAuthorizedTerminal(
        const DeviceEraseJournalEntry& terminal);
};

}  // namespace eidolon

#endif
