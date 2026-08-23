#include "device_local_erase_core.h"

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
    return std::string("{\"accepted_manifest_digest\":") +
           Quote(ref.accepted_manifest_digest) +
           ",\"claim_generation\":" + std::to_string(ref.claim_generation) +
           ",\"device_instance_id\":" + Quote(ref.device_instance_id) +
           ",\"owner_domain_generation\":" +
           std::to_string(ref.owner_domain_generation) +
           ",\"owner_domain_id\":" + Quote(ref.owner_domain_id) +
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
    return left.device_instance_id == right.device_instance_id &&
           left.owner_domain_id == right.owner_domain_id &&
           left.owner_domain_generation == right.owner_domain_generation &&
           left.claim_generation == right.claim_generation &&
           left.trust_epoch == right.trust_epoch &&
           left.accepted_manifest_digest == right.accepted_manifest_digest;
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

DeviceEraseCoreOutcome DeviceLocalEraseCore::Handle(
    const device_foundation::v1::DeviceLocalEraseCommand& command,
    const std::string& request_fingerprint) {
    DeviceEraseJournalEntry stored;
    if (journal_.Load(stored) && stored.operation_id == command.operation_id) {
        if (stored.request_fingerprint != request_fingerprint ||
            !SameRef(stored.device_ref, command.device_ref)) {
            return {DeviceEraseCoreResult::IdempotencyConflict, false, {}};
        }
        if (stored.terminal) {
            return {DeviceEraseCoreResult::Replayed, true, stored.terminal_ack};
        }
    }
    if (!SameRef(current_ref_, command.device_ref)) {
        return {DeviceEraseCoreResult::StaleGeneration, false, {}};
    }
    if (clock_.DeadlineExpired(command.deadline)) {
        return {DeviceEraseCoreResult::Expired, false, {}};
    }

    DeviceEraseJournalEntry accepted{
        command.operation_id, request_fingerprint, command.device_ref, false, {}};
    if (!journal_.Store(accepted)) {
        return {DeviceEraseCoreResult::StorageFailure, false, {}};
    }
    const DeviceEraseAdapterOutcome result = adapter_.EraseOwnerState(command);
    device_foundation::v1::DeviceLocalEraseAck ack;
    ack.operation_id = command.operation_id;
    ack.device_ref = command.device_ref;
    ack.ack_sequence = 1;
    ack.result = result.result == DeviceEraseAdapterResult::Erased
                     ? device_foundation::v1::DeviceLocalEraseResult::Erased
                     : device_foundation::v1::DeviceLocalEraseResult::PermanentFailure;
    ack.result_code = result.result_code;
    ack.device_monotonic_time = clock_.MonotonicTime();
    if (!signer_.SignCanonical(AckSigningDocument(ack), ack.device_signature)) {
        return {DeviceEraseCoreResult::SignatureFailure, false, {}};
    }
    accepted.terminal = true;
    accepted.terminal_ack = ack;
    if (!journal_.Store(accepted)) {
        return {DeviceEraseCoreResult::StorageFailure, false, {}};
    }
    return {DeviceEraseCoreResult::Acknowledged, true, ack};
}

}  // namespace eidolon
