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

// Crash-safe adapter coordinating the legacy Wi-Fi profile store and the
// fail-safe Owner trust slots. The journal contains only generation, Owner,
// SSID and a credential digest; it never stores a replayable password.
CommissioningTransactionResult CommitCommissioningTransaction(
    uint32_t generation, const std::string& ssid, const std::string& password);

// Runs before Station starts. Returns false only when a durable transaction is
// still pending and normal networking must not race it.
bool RecoverPendingCommissioningTransaction();

// Rollback is permitted only before the network profile became durable.
bool RollbackPendingCommissioningTransaction(uint32_t generation);

}  // namespace eidolon

#endif  // EIDOLON_COMMISSIONING_TRANSACTION_H_
