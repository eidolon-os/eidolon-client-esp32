#include "device_delivery_consumer_core.h"

#include "device_ref_access.h"

#include <cJSON.h>

#include <cstdio>
#include <limits>
#include <utility>

namespace eidolon {
namespace {

using device_foundation::v1::DeliverEnvelope;
using device_foundation::v1::DeliveryAcceptanceState;
using device_foundation::v1::DeliveryKind;
using device_foundation::v1::DeviceEvidenceEnvelope;
using device_foundation::v1::DeviceLocalEraseAck;
using device_foundation::v1::DeviceLocalEraseCommand;
using device_foundation::v1::DeviceRef;

std::string Text(const cJSON* object, const char* key) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

bool ExactObject(const cJSON* object, int size) {
    return cJSON_IsObject(object) && cJSON_GetArraySize(object) == size;
}

bool Unsigned(const cJSON* object, const char* key, uint64_t& out) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < 1 ||
        item->valuedouble > 9007199254740991.0) {
        return false;
    }
    const uint64_t value = static_cast<uint64_t>(item->valuedouble);
    if (static_cast<double>(value) != item->valuedouble) return false;
    out = value;
    return true;
}

bool Identifier(const std::string& value) {
    if (value.size() < 3 || value.size() > 128) return false;
    for (const unsigned char ch : value) {
        const bool alpha_numeric =
            (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z');
        if (!alpha_numeric && ch != '.' && ch != '_' && ch != ':' &&
            ch != '-') {
            return false;
        }
    }
    return true;
}

bool ParseDeviceRef(const cJSON* value, DeviceRef& out) {
    uint64_t claim = 0;
    uint64_t trust = 0;
    if (!ExactObject(value, 5) ||
        !Unsigned(value, "owner_domain_generation", out.owner_domain_generation) ||
        !Unsigned(value, "claim_generation", claim) ||
        !Unsigned(value, "trust_epoch", trust) ||
        claim > std::numeric_limits<uint32_t>::max() ||
        trust > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    out.device_instance_id = Text(value, "device_instance_id");
    out.owner_domain_id.value = Text(value, "owner_domain_id");
    out.claim_generation = static_cast<uint32_t>(claim);
    out.trust_epoch = static_cast<uint32_t>(trust);
    return Identifier(out.device_instance_id) &&
           out.owner_domain_id.value.rfind("owner-", 0) == 0 &&
           Identifier(out.owner_domain_id.value);
}

bool Rfc3339(const std::string& value) {
    return value.size() >= 20 && value[4] == '-' && value[7] == '-' &&
           value[10] == 'T' && value[13] == ':' && value[16] == ':' &&
           (value.back() == 'Z' || value.find('+', 19) != std::string::npos ||
            value.find('-', 19) != std::string::npos);
}

bool ParseEraseCommand(const cJSON* value, DeviceLocalEraseCommand& out) {
    if (!ExactObject(value, 7) ||
        Text(value, "contract") !=
            "eidolon.device-foundation.device-operation" ||
        Text(value, "contract_version") != "1.0" ||
        Text(value, "operation_type") != "device-local.erase") {
        return false;
    }
    out.operation_id = Text(value, "operation_id");
    out.deadline = Text(value, "deadline");
    const cJSON* ref = cJSON_GetObjectItemCaseSensitive(value, "device_ref");
    const cJSON* payload = cJSON_GetObjectItemCaseSensitive(value, "payload");
    const cJSON* scopes =
        cJSON_GetObjectItemCaseSensitive(payload, "erase_scopes");
    if (!Identifier(out.operation_id) || !Rfc3339(out.deadline) ||
        !ParseDeviceRef(ref, out.device_ref) || !ExactObject(payload, 1) ||
        !cJSON_IsArray(scopes) || cJSON_GetArraySize(scopes) < 1 ||
        cJSON_GetArraySize(scopes) > 3) {
        return false;
    }
    bool credentials = false;
    bool data = false;
    bool network = false;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, scopes) {
        if (!cJSON_IsString(item) || !item->valuestring) return false;
        const std::string scope = item->valuestring;
        bool* seen = scope == "owner-credentials" ? &credentials
                     : scope == "owner-data"       ? &data
                     : scope == "network-profiles" ? &network
                                                    : nullptr;
        if (seen == nullptr || *seen) return false;
        *seen = true;
        out.erase_scopes.push_back(scope);
    }
    return true;
}

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
    return out + '\"';
}

std::string DeviceRefJson(const DeviceRef& ref) {
    return std::string("{\"claim_generation\":") +
           std::to_string(ref.claim_generation) +
           ",\"device_instance_id\":" + Quote(ref.device_instance_id) +
           ",\"owner_domain_generation\":" +
           std::to_string(ref.owner_domain_generation) +
           ",\"owner_domain_id\":" + Quote(DeviceRefOwnerDomainId(ref)) +
           ",\"trust_epoch\":" + std::to_string(ref.trust_epoch) + "}";
}

DeviceDeliveryConsumerOutcome Rejected(const std::string& attempt_id,
                                       DeviceDeliveryConsumerResult result,
                                       const std::string& code) {
    DeviceDeliveryConsumerOutcome outcome;
    outcome.result = result;
    outcome.acceptance.delivery_attempt_id = attempt_id.empty()
                                                 ? "unknown-attempt"
                                                 : attempt_id;
    outcome.acceptance.state = DeliveryAcceptanceState::Rejected;
    outcome.acceptance.adapter_code = code;
    return outcome;
}

}  // namespace

DeviceDeliveryConsumerOutcome DeviceDeliveryConsumerCore::Handle(
    const std::string& envelope_json) {
    cJSON* root = cJSON_ParseWithLength(envelope_json.data(), envelope_json.size());
    const std::string attempt = Text(root, "delivery_attempt_id");
    DeliverEnvelope envelope;
    DeviceLocalEraseCommand command;
    const cJSON* ref = cJSON_GetObjectItemCaseSensitive(root, "device_ref");
    const cJSON* payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    const bool valid = ExactObject(root, 7) && Identifier(attempt) &&
        Identifier(Text(root, "message_id")) &&
        Text(root, "kind") == "operation" &&
        Rfc3339(Text(root, "deadline")) &&
        Text(root, "payload_schema") == kDeviceLocalEraseCommandSchema &&
        ParseDeviceRef(ref, envelope.device_ref) &&
        ParseEraseCommand(payload, command);
    if (!valid) {
        cJSON_Delete(root);
        return Rejected(attempt, DeviceDeliveryConsumerResult::RejectedInvalidContract,
                        "INVALID_CONTRACT");
    }
    envelope.delivery_attempt_id = attempt;
    envelope.message_id = Text(root, "message_id");
    envelope.kind = DeliveryKind::Operation;
    envelope.deadline = Text(root, "deadline");
    envelope.payload_schema = Text(root, "payload_schema");
    char* printed = cJSON_PrintUnformatted(payload);
    envelope.payload_json = printed ? printed : "";
    cJSON_free(printed);
    cJSON_Delete(root);

    if (envelope.message_id != command.operation_id ||
        envelope.deadline != command.deadline ||
        !SameDeviceRef(envelope.device_ref, command.device_ref)) {
        return Rejected(attempt, DeviceDeliveryConsumerResult::RejectedInvalidContract,
                        "ENVELOPE_PAYLOAD_MISMATCH");
    }
    std::string fingerprint;
    if (!fingerprint_.Sha256(DeviceLocalEraseCore::CommandDocument(command), fingerprint)) {
        return Rejected(attempt, DeviceDeliveryConsumerResult::RetryableFailure,
                        "FINGERPRINT_UNAVAILABLE");
    }
    const DeviceEraseCoreOutcome erased = erase_.Handle(command, fingerprint);
    if (erased.result == DeviceEraseCoreResult::StaleGeneration) {
        return Rejected(attempt, DeviceDeliveryConsumerResult::RejectedStaleGeneration,
                        "STALE_GENERATION");
    }
    if (erased.result == DeviceEraseCoreResult::Expired) {
        return Rejected(attempt, DeviceDeliveryConsumerResult::RejectedExpired,
                        "DEADLINE_EXPIRED");
    }
    if (erased.result == DeviceEraseCoreResult::ClockUntrusted) {
        // Ask again, do not decide. Answering "expired" here is what made a
        // week-long deadline unusable to a device that had simply not synced
        // its clock yet — the Authority kept re-arming the delivery and this
        // side kept refusing it in words that said it never could.
        return Rejected(attempt, DeviceDeliveryConsumerResult::RetryableFailure,
                        "CLOCK_UNTRUSTED");
    }
    if (erased.result == DeviceEraseCoreResult::IdempotencyConflict ||
        erased.result == DeviceEraseCoreResult::OperationConflict) {
        return Rejected(attempt, DeviceDeliveryConsumerResult::RejectedInvalidContract,
                        "OPERATION_CONFLICT");
    }

    DeviceDeliveryConsumerOutcome outcome;
    outcome.acceptance.delivery_attempt_id = attempt;
    outcome.acceptance.state = DeliveryAcceptanceState::Accepted;
    if (!erased.has_ack) {
        outcome.result = DeviceDeliveryConsumerResult::RetryableFailure;
        return outcome;
    }
    outcome.result = DeviceDeliveryConsumerResult::EvidenceReady;
    outcome.has_evidence = true;
    outcome.evidence.delivery_attempt_id = attempt;
    outcome.evidence.message_id = command.operation_id;
    outcome.evidence.device_ref = command.device_ref;
    outcome.evidence.payload_schema = kDeviceLocalEraseAckSchema;
    outcome.evidence.payload_json = DeviceLocalEraseCore::AckSigningDocument(erased.ack);
    outcome.evidence.payload_json.insert(
        outcome.evidence.payload_json.size() - 1,
        ",\"device_signature\":" + Quote(erased.ack.device_signature));
    // The signing document excludes the signature; transport evidence carries
    // that exact signed document plus the signature, without interpreting it.
    outcome.evidence_json = EvidenceJson(outcome.evidence);
    return outcome;
}

std::string DeviceDeliveryConsumerCore::EvidenceJson(
    const DeviceEvidenceEnvelope& envelope) {
    return std::string("{\"delivery_attempt_id\":") +
           Quote(envelope.delivery_attempt_id) +
           ",\"device_ref\":" + DeviceRefJson(envelope.device_ref) +
           ",\"kind\":\"operation_ack\"" +
           ",\"message_id\":" + Quote(envelope.message_id) +
           ",\"payload\":" + envelope.payload_json +
           ",\"payload_schema\":" + Quote(envelope.payload_schema) + "}";
}

}  // namespace eidolon
