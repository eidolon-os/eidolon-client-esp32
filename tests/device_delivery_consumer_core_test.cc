#include "eidolon/device_delivery_consumer_core.h"

#include <cJSON.h>

#include <cassert>
#include <string>

using namespace eidolon;
using namespace eidolon::device_foundation::v1;

namespace {

DeviceRef Ref(uint32_t generation = 7) {
    return {"device_erase_01", {"owner-domain_01"}, 3, generation, 4};
}

const char* Envelope(uint32_t generation = 7) {
    return generation == 7 ? R"JSON({
      "delivery_attempt_id":"erase_attempt_01",
      "message_id":"erase_operation_01",
      "kind":"operation",
      "device_ref":{"device_instance_id":"device_erase_01","owner_domain_id":"owner-domain_01","owner_domain_generation":3,"claim_generation":7,"trust_epoch":4},
      "deadline":"2026-08-30T00:00:00Z",
      "payload_schema":"https://contracts.eidolon.live/device-foundation/v1/device-control/schemas.schema.json#/$defs/DeviceLocalEraseCommand",
      "payload":{"contract":"eidolon.device-foundation.device-operation","contract_version":"1.0","deadline":"2026-08-30T00:00:00Z","device_ref":{"device_instance_id":"device_erase_01","owner_domain_id":"owner-domain_01","owner_domain_generation":3,"claim_generation":7,"trust_epoch":4},"operation_id":"erase_operation_01","operation_type":"device-local.erase","payload":{"erase_scopes":["owner-credentials","owner-data","network-profiles"]}}
    })JSON" : R"JSON({
      "delivery_attempt_id":"erase_attempt_01",
      "message_id":"erase_operation_01",
      "kind":"operation",
      "device_ref":{"device_instance_id":"device_erase_01","owner_domain_id":"owner-domain_01","owner_domain_generation":3,"claim_generation":6,"trust_epoch":4},
      "deadline":"2026-08-30T00:00:00Z",
      "payload_schema":"https://contracts.eidolon.live/device-foundation/v1/device-control/schemas.schema.json#/$defs/DeviceLocalEraseCommand",
      "payload":{"contract":"eidolon.device-foundation.device-operation","contract_version":"1.0","deadline":"2026-08-30T00:00:00Z","device_ref":{"device_instance_id":"device_erase_01","owner_domain_id":"owner-domain_01","owner_domain_generation":3,"claim_generation":6,"trust_epoch":4},"operation_id":"erase_operation_01","operation_type":"device-local.erase","payload":{"erase_scopes":["owner-credentials","owner-data","network-profiles"]}}
    })JSON";
}

class Journal final : public DeviceEraseJournalPort {
public:
    DeviceEraseJournalLoadResult Load(DeviceEraseJournalEntry& out) override {
        if (!present) return DeviceEraseJournalLoadResult::NotFound;
        out = value;
        return DeviceEraseJournalLoadResult::Loaded;
    }
    bool Store(const DeviceEraseJournalEntry& next) override {
        value = next;
        present = true;
        return true;
    }
    bool present = false;
    DeviceEraseJournalEntry value;
};

class Adapter final : public DeviceLocalEraseAdapterPort {
public:
    DeviceEraseAdapterOutcome PrepareOwnerState(
        const DeviceLocalEraseCommand&) override {
        ++prepare_calls;
        return prepare;
    }
    DeviceEraseAdapterOutcome FinalizeOwnerState(
        const DeviceLocalEraseCommand&) override {
        ++finalize_calls;
        return finalize;
    }
    int prepare_calls = 0;
    int finalize_calls = 0;
    DeviceEraseAdapterOutcome prepare{
        DeviceEraseAdapterResult::PreparedForFinalization, "PREPARED"};
    DeviceEraseAdapterOutcome finalize{
        DeviceEraseAdapterResult::Erased, "ERASED"};
};

class Clock final : public DeviceEraseClockPort {
public:
    bool DeadlineExpired(const std::string&) const override { return expired; }
    uint64_t MonotonicTime() const override { return 1234; }
    bool expired = false;
};

class Signer final : public DeviceEraseAckSignerPort {
public:
    bool SignCanonical(const std::string&, std::string& signature) override {
        ++calls;
        signature = std::string(86, 'A');
        return true;
    }
    int calls = 0;
};

class Fingerprint final : public DeviceDeliveryFingerprintPort {
public:
    bool Sha256(const std::string& canonical, std::string& digest) override {
        ++calls;
        last = canonical;
        if (fail) return false;
        digest = "sha256:" + std::string(64, '1');
        return true;
    }
    bool fail = false;
    int calls = 0;
    std::string last;
};

struct Fixture {
    Journal journal;
    Adapter adapter;
    Clock clock;
    Signer signer;
    Fingerprint fingerprint;
    DeviceLocalEraseCore erase{Ref(), journal, adapter, clock, signer};
    DeviceDeliveryConsumerCore delivery{erase, fingerprint};
};

void CanonicalEnvelopeExecutesAndReturnsSignedEvidence() {
    Fixture f;
    const auto outcome = f.delivery.Handle(Envelope());
    assert(outcome.result == DeviceDeliveryConsumerResult::EvidenceReady);
    assert(outcome.acceptance.state == DeliveryAcceptanceState::Accepted);
    assert(outcome.acceptance.adapter_code.empty());
    assert(outcome.has_evidence);
    assert(outcome.evidence.message_id == "erase_operation_01");
    assert(outcome.evidence.payload_schema == kDeviceLocalEraseAckSchema);
    assert(f.adapter.prepare_calls == 1);
    assert(f.adapter.finalize_calls == 1);
    assert(f.fingerprint.last ==
           "{\"contract\":\"eidolon.device-foundation.device-operation\",\"contract_version\":\"1.0\",\"deadline\":\"2026-08-30T00:00:00Z\",\"device_ref\":{\"claim_generation\":7,\"device_instance_id\":\"device_erase_01\",\"owner_domain_generation\":3,\"owner_domain_id\":\"owner-domain_01\",\"trust_epoch\":4},\"operation_id\":\"erase_operation_01\",\"operation_type\":\"device-local.erase\",\"payload\":{\"erase_scopes\":[\"owner-credentials\",\"owner-data\",\"network-profiles\"]}}");

    cJSON* evidence = cJSON_Parse(outcome.evidence_json.c_str());
    assert(evidence != nullptr);
    assert(std::string(cJSON_GetStringValue(
               cJSON_GetObjectItemCaseSensitive(evidence, "kind"))) ==
           "operation_ack");
    const cJSON* payload =
        cJSON_GetObjectItemCaseSensitive(evidence, "payload");
    assert(std::string(cJSON_GetStringValue(
               cJSON_GetObjectItemCaseSensitive(payload, "device_signature"))) ==
           std::string(86, 'A'));
    cJSON_Delete(evidence);
}

void ReplayReturnsSameEvidenceWithoutSecondErase() {
    Fixture f;
    const auto first = f.delivery.Handle(Envelope());
    const auto replay = f.delivery.Handle(Envelope());
    assert(first.evidence_json == replay.evidence_json);
    assert(f.adapter.prepare_calls == 1);
    assert(f.adapter.finalize_calls == 1);
    assert(f.signer.calls == 1);
}

void RetryableEraseNeverProducesSuccessEvidence() {
    Fixture f;
    f.adapter.prepare = {
        DeviceEraseAdapterResult::RetryableStorageFailure,
        "RETRYABLE_STORAGE_FAILURE"};
    const auto outcome = f.delivery.Handle(Envelope());
    assert(outcome.result == DeviceDeliveryConsumerResult::RetryableFailure);
    assert(outcome.acceptance.state == DeliveryAcceptanceState::Accepted);
    assert(!outcome.has_evidence);
    assert(outcome.evidence_json.empty());
    assert(f.signer.calls == 0);
}

void OldGenerationAndExpiredDeliveryFailClosed() {
    Fixture stale;
    const auto old = stale.delivery.Handle(Envelope(6));
    assert(old.result == DeviceDeliveryConsumerResult::RejectedStaleGeneration);
    assert(old.acceptance.adapter_code == "STALE_GENERATION");
    assert(stale.adapter.prepare_calls == 0);

    Fixture expired;
    expired.clock.expired = true;
    const auto late = expired.delivery.Handle(Envelope());
    assert(late.result == DeviceDeliveryConsumerResult::RejectedExpired);
    assert(expired.adapter.prepare_calls == 0);
}

void EnvelopePayloadMismatchAndUnknownFieldsAreRejected() {
    Fixture f;
    std::string mismatch = Envelope();
    const std::string old_message = "erase_operation_01";
    const auto position = mismatch.find(old_message);
    mismatch.replace(position, old_message.size(), "different_operation");
    assert(f.delivery.Handle(mismatch).acceptance.adapter_code ==
           "ENVELOPE_PAYLOAD_MISMATCH");

    std::string extra = Envelope();
    extra.insert(extra.rfind('}'), ",\"terminal_result\":\"erased\"");
    assert(f.delivery.Handle(extra).acceptance.adapter_code == "INVALID_CONTRACT");
    assert(f.adapter.prepare_calls == 0);
}

void FingerprintFailureDoesNotEnterDestructiveCore() {
    Fixture f;
    f.fingerprint.fail = true;
    const auto outcome = f.delivery.Handle(Envelope());
    assert(outcome.result == DeviceDeliveryConsumerResult::RetryableFailure);
    assert(outcome.acceptance.adapter_code == "FINGERPRINT_UNAVAILABLE");
    assert(f.adapter.prepare_calls == 0);
}

}  // namespace

int main() {
    CanonicalEnvelopeExecutesAndReturnsSignedEvidence();
    ReplayReturnsSameEvidenceWithoutSecondErase();
    RetryableEraseNeverProducesSuccessEvidence();
    OldGenerationAndExpiredDeliveryFailClosed();
    EnvelopePayloadMismatchAndUnknownFieldsAreRejected();
    FingerprintFailureDoesNotEnterDestructiveCore();
    return 0;
}
