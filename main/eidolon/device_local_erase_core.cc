#include "device_local_erase_core.h"

#include "device_ref_access.h"

#include <cstdio>
#include <utility>

namespace eidolon {
namespace {

std::string Quote(const std::string& value) {
    std::string out = "\"";
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20) {
                char escaped[7];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
                out += escaped;
            } else {
                out += static_cast<char>(ch);
            }
        }
    }
    out += '\"';
    return out;
}

std::string DeviceRefJson(const device_foundation::v1::DeviceRef& ref) {
    return std::string("{\"claim_generation\":") +
           std::to_string(ref.claim_generation) +
           ",\"device_instance_id\":" + Quote(ref.device_instance_id) +
           ",\"owner_domain_generation\":" +
           std::to_string(ref.owner_domain_generation) +
           ",\"owner_domain_id\":" + Quote(DeviceRefOwnerDomainId(ref)) +
           ",\"trust_epoch\":" + std::to_string(ref.trust_epoch) + "}";
}

const char* ResultWire(device_foundation::v1::DeviceLocalEraseResult result) {
    return result == device_foundation::v1::DeviceLocalEraseResult::Erased
               ? "erased"
               : "permanent-failure";
}

}  // namespace

DeviceLocalEraseCore::DeviceLocalEraseCore(
    device_foundation::v1::DeviceRef current_ref,
    DeviceEraseJournalPort& journal,
    DeviceLocalEraseAdapterPort& adapter,
    DeviceEraseClockPort& clock,
    DeviceEraseAckSignerPort& signer)
    : current_ref_(std::move(current_ref)),
      journal_(journal),
      adapter_(adapter),
      clock_(clock),
      signer_(signer) {}

bool DeviceLocalEraseCore::SameRef(
    const device_foundation::v1::DeviceRef& left,
    const device_foundation::v1::DeviceRef& right) {
    return SameDeviceRef(left, right);
}

std::string DeviceLocalEraseCore::AckSigningDocument(
    const device_foundation::v1::DeviceLocalEraseAck& ack) {
    return std::string("{\"ack_sequence\":") +
           std::to_string(ack.ack_sequence) +
           ",\"contract\":\"eidolon.device-foundation.device-operation-ack\"" +
           ",\"contract_version\":\"1.0\"" +
           ",\"device_monotonic_time\":" +
           std::to_string(ack.device_monotonic_time) +
           ",\"device_ref\":" + DeviceRefJson(ack.device_ref) +
           ",\"operation_id\":" + Quote(ack.operation_id) +
           ",\"operation_type\":\"device-local.erase\"" +
           ",\"result\":" + Quote(ResultWire(ack.result)) +
           ",\"result_code\":" + Quote(ack.result_code) + "}";
}

std::string DeviceLocalEraseCore::CommandDocument(
    const device_foundation::v1::DeviceLocalEraseCommand& command) {
    std::string scopes = "[";
    for (size_t index = 0; index < command.erase_scopes.size(); ++index) {
        if (index != 0) scopes += ',';
        scopes += Quote(command.erase_scopes[index]);
    }
    scopes += ']';
    return std::string("{\"contract\":\"eidolon.device-foundation.device-operation\"") +
           ",\"contract_version\":\"1.0\"" +
           ",\"deadline\":" + Quote(command.deadline) +
           ",\"device_ref\":" + DeviceRefJson(command.device_ref) +
           ",\"operation_id\":" + Quote(command.operation_id) +
           ",\"operation_type\":\"device-local.erase\"" +
           ",\"payload\":{\"erase_scopes\":" + scopes + "}}";
}

DeviceEraseCoreOutcome DeviceLocalEraseCore::Handle(
    const device_foundation::v1::DeviceLocalEraseCommand& command,
    const std::string& request_fingerprint) {
    DeviceEraseJournalEntry stored;
    const DeviceEraseJournalLoadResult load_result = journal_.Load(stored);
    if (load_result == DeviceEraseJournalLoadResult::StorageFailure) {
        return {DeviceEraseCoreResult::StorageFailure, false, {}};
    }
    bool journal_present =
        load_result == DeviceEraseJournalLoadResult::Loaded;
    if (journal_present) {
        const bool valid_phase =
            stored.phase == DeviceEraseJournalPhase::Accepted ||
            stored.phase == DeviceEraseJournalPhase::Staging ||
            stored.phase == DeviceEraseJournalPhase::Erasing ||
            stored.phase == DeviceEraseJournalPhase::DurableTerminal;
        const bool ack_phase_valid =
            !stored.has_staged_ack ||
            stored.phase == DeviceEraseJournalPhase::Erasing ||
            stored.phase == DeviceEraseJournalPhase::DurableTerminal;
        const bool ack_valid =
            !stored.has_staged_ack ||
            (stored.staged_ack.operation_id == stored.operation_id &&
             SameRef(stored.staged_ack.device_ref, stored.device_ref) &&
             stored.staged_ack.ack_sequence != 0 &&
             !stored.staged_ack.result_code.empty() &&
             !stored.staged_ack.device_signature.empty());
        if (!valid_phase || !ack_phase_valid || !ack_valid ||
            (stored.phase == DeviceEraseJournalPhase::DurableTerminal &&
             !stored.has_staged_ack)) {
            return {DeviceEraseCoreResult::StorageFailure, false, {}};
        }
    }
    if (journal_present && stored.operation_id == command.operation_id) {
        if (stored.request_fingerprint != request_fingerprint ||
            !SameRef(stored.device_ref, command.device_ref) ||
            stored.deadline != command.deadline ||
            stored.erase_scopes != command.erase_scopes) {
            return {DeviceEraseCoreResult::IdempotencyConflict, false, {}};
        }
        if (stored.phase == DeviceEraseJournalPhase::DurableTerminal) {
            return {DeviceEraseCoreResult::Replayed, true, stored.staged_ack};
        }
    } else if (journal_present &&
               stored.phase == DeviceEraseJournalPhase::DurableTerminal &&
               !SameRef(stored.device_ref, command.device_ref)) {
        // A later Claim/instance may own a fresh removal operation after the
        // previous local result is durable. current_ref_ validation below keeps
        // a delayed old or forged DeviceRef from using this rotation point.
        journal_present = false;
        stored = {};
    } else if (journal_present) {
        // A single durable RemovalJournal owns the destructive local workflow.
        // Never overwrite another operation, including its replay evidence.
        return {DeviceEraseCoreResult::OperationConflict, false, {}};
    }
    if (!SameRef(current_ref_, command.device_ref)) {
        return {DeviceEraseCoreResult::StaleGeneration, false, {}};
    }
    const bool destructive_resume =
        journal_present && stored.phase == DeviceEraseJournalPhase::Erasing;
    if (!destructive_resume && clock_.DeadlineExpired(command.deadline)) {
        return {DeviceEraseCoreResult::Expired, false, {}};
    }

    DeviceEraseJournalEntry entry = stored;
    if (!journal_present) {
        entry.operation_id = command.operation_id;
        entry.request_fingerprint = request_fingerprint;
        entry.device_ref = command.device_ref;
        entry.deadline = command.deadline;
        entry.erase_scopes = command.erase_scopes;
        entry.phase = DeviceEraseJournalPhase::Accepted;
        if (!journal_.Store(entry)) {
            return {DeviceEraseCoreResult::StorageFailure, false, {}};
        }
    }
    if (entry.phase == DeviceEraseJournalPhase::Accepted) {
        entry.phase = DeviceEraseJournalPhase::Staging;
        if (!journal_.Store(entry)) {
            return {DeviceEraseCoreResult::StorageFailure, false, {}};
        }
    }
    if (entry.phase == DeviceEraseJournalPhase::Staging) {
        // This committed marker is the destructive point of no return. Once it
        // exists, the same generation-bound operation resumes past its delivery
        // deadline until a durable terminal result exists.
        entry.phase = DeviceEraseJournalPhase::Erasing;
        if (!journal_.Store(entry)) {
            return {DeviceEraseCoreResult::StorageFailure, false, {}};
        }
    }

    const auto terminal_ack = [&](const DeviceEraseAdapterOutcome& outcome,
                                  bool erased) -> DeviceEraseCoreOutcome {
        device_foundation::v1::DeviceLocalEraseAck ack;
        ack.operation_id = command.operation_id;
        ack.device_ref = command.device_ref;
        ack.ack_sequence = 1;
        ack.result = erased
                         ? device_foundation::v1::DeviceLocalEraseResult::Erased
                         : device_foundation::v1::DeviceLocalEraseResult::PermanentFailure;
        ack.result_code = outcome.result_code;
        ack.device_monotonic_time = clock_.MonotonicTime();
        if (!signer_.SignCanonical(AckSigningDocument(ack), ack.device_signature)) {
            return {DeviceEraseCoreResult::SignatureFailure, false, {}};
        }
        entry.has_staged_ack = true;
        entry.staged_ack = ack;
        entry.phase = DeviceEraseJournalPhase::DurableTerminal;
        if (!journal_.Store(entry)) {
            return {DeviceEraseCoreResult::StorageFailure, false, {}};
        }
        return {DeviceEraseCoreResult::Acknowledged, true, ack};
    };

    if (!entry.has_staged_ack) {
        const DeviceEraseAdapterOutcome prepared =
            adapter_.PrepareOwnerState(command);
        if (prepared.result == DeviceEraseAdapterResult::RetryableStorageFailure) {
            return {DeviceEraseCoreResult::RetryableStorageFailure, false, {}};
        }
        if (prepared.result !=
            DeviceEraseAdapterResult::PreparedForFinalization) {
            return terminal_ack(prepared, false);
        }

        device_foundation::v1::DeviceLocalEraseAck ack;
        ack.operation_id = command.operation_id;
        ack.device_ref = command.device_ref;
        ack.ack_sequence = 1;
        ack.result = device_foundation::v1::DeviceLocalEraseResult::Erased;
        ack.result_code = "ERASED";
        ack.device_monotonic_time = clock_.MonotonicTime();
        if (!signer_.SignCanonical(AckSigningDocument(ack), ack.device_signature)) {
            return {DeviceEraseCoreResult::SignatureFailure, false, {}};
        }
        entry.has_staged_ack = true;
        entry.staged_ack = ack;
        if (!journal_.Store(entry)) {
            return {DeviceEraseCoreResult::StorageFailure, false, {}};
        }
    }

    const DeviceEraseAdapterOutcome finalized =
        adapter_.FinalizeOwnerState(command);
    if (finalized.result == DeviceEraseAdapterResult::RetryableStorageFailure) {
        return {DeviceEraseCoreResult::RetryableStorageFailure, false, {}};
    }
    if (finalized.result != DeviceEraseAdapterResult::Erased) {
        return terminal_ack(finalized, false);
    }
    entry.phase = DeviceEraseJournalPhase::DurableTerminal;
    if (!journal_.Store(entry)) {
        return {DeviceEraseCoreResult::StorageFailure, false, {}};
    }
    return {DeviceEraseCoreResult::Acknowledged, true, entry.staged_ack};
}

DeviceEraseCoreOutcome DeviceLocalEraseCore::ResumePending() {
    DeviceEraseJournalEntry stored;
    const DeviceEraseJournalLoadResult load_result = journal_.Load(stored);
    if (load_result == DeviceEraseJournalLoadResult::StorageFailure) {
        return {DeviceEraseCoreResult::StorageFailure, false, {}};
    }
    if (load_result == DeviceEraseJournalLoadResult::NotFound) {
        return {DeviceEraseCoreResult::NoPendingOperation, false, {}};
    }
    if (stored.deadline.empty() || stored.erase_scopes.empty()) {
        return {DeviceEraseCoreResult::StorageFailure, false, {}};
    }
    device_foundation::v1::DeviceLocalEraseCommand command;
    command.operation_id = stored.operation_id;
    command.device_ref = stored.device_ref;
    command.deadline = stored.deadline;
    command.erase_scopes = stored.erase_scopes;
    return Handle(command, stored.request_fingerprint);
}

}  // namespace eidolon
