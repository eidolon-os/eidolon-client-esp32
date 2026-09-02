#ifndef EIDOLON_DEVICE_PHYSICAL_RECOVERY_H_
#define EIDOLON_DEVICE_PHYSICAL_RECOVERY_H_

#include "device_local_erase_core.h"

namespace eidolon {

class DevicePhysicalRecovery {
public:
    // Read-only. Answers the one question the advertising decision has to ask
    // before it opens a setup window: is there a removal on record that only
    // physical presence may consume? Changes nothing, so it is safe to call
    // from the intent boundary on every request.
    static bool RemovalBlocksCommissioning();
    // Called only from the explicit long-press path. If there is no removal
    // terminal it is a no-op; otherwise it durably authorizes and runs rejoin.
    static bool AuthorizeFromPhysicalPresence();
    // Boot may resume a transaction only after its authorization was durable.
    static bool ResumeAuthorizedTerminal(
        const DeviceEraseJournalEntry& terminal);

private:
    // The no-removal-journal branch of the physical-presence gesture: an Owner
    // revocation is terminal without one, and this is how a person at the
    // device gives that Claim up.
    static bool ClearRevokedClaimOnPhysicalPresence();
};

}  // namespace eidolon

#endif
