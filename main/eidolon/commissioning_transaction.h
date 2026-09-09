#ifndef EIDOLON_COMMISSIONING_TRANSACTION_H_
#define EIDOLON_COMMISSIONING_TRANSACTION_H_

#include <cstdint>
#include <string>

namespace eidolon {

enum class CommissioningTransactionResult {
    Committed,
    SafeFailure,
    RecoveryRequired,
};

// One commissioning transaction coordinates Wi-Fi, scoped Owner cleanup,
// identity/credential publication and the existing trust slots. Its journal
// holds phases and snapshot digests; the password stays in the Wi-Fi store.
CommissioningTransactionResult CommitCommissioningTransaction(
    uint32_t generation, const std::string& ssid, const std::string& password);

// Runs before Station starts. Returns false only when a durable transaction is
// still pending and normal networking must not race it.
bool RecoverPendingCommissioningTransaction();

// An interrupted legacy transaction has no identity snapshot. A physically
// opened, authenticated setup may replace it using a freshly issued identity.
// A current durable transaction must finish before another setup can start.
bool CommissioningTransactionAllowsNewSetup();
bool CommissioningTransactionNeedsFreshIdentity();

// Rollback is permitted only before the durable commit decision.
bool RollbackPendingCommissioningTransaction(uint32_t generation);

}  // namespace eidolon

#endif  // EIDOLON_COMMISSIONING_TRANSACTION_H_
