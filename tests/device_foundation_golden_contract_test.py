import hashlib
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests/fixtures/device_foundation_v1"


def canonical(value: object) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)


def main() -> None:
    header = (ROOT / "main/eidolon/device_foundation_v1_generated.h").read_text()
    match = re.search(r"struct DeviceRef \{(.*?)\n\};", header, re.S)
    assert match is not None
    fields = re.findall(r"\b([a-z][a-z0-9_]*)\s*(?:=|;)", match.group(1))
    assert fields == [
        "device_instance_id",
        "owner_domain_id",
        "owner_domain_generation",
        "claim_generation",
        "trust_epoch",
    ]
    assert "struct OwnerDomainId" in header
    assert "struct BusinessOwnerId" in header
    assert "struct ClaimGrantAAD" in header
    assert "struct ClaimGrantWireEnvelope" in header
    assert (
        "struct CollectClaimGrantResult { std::string grant_id; "
        "ClaimGrantWireEnvelope wire_envelope;" in header
    )

    admission = json.loads((FIXTURES / "admission.valid.json").read_text())
    decide = next(
        case["value"]
        for case in admission["cases"]
        if case["case_id"] == "DF-ADMISSION-DECIDE-VALID"
    )
    assert decide["target_owner_domain_id"] == "owner-domain_01"
    assert decide["target_business_owner_id"] == "owner_01"
    assert decide["target_owner_domain_id"] != decide["target_business_owner_id"]
    decided = next(
        case["value"]
        for case in admission["cases"]
        if case["case_id"] == "DF-ADMISSION-DECIDE-RESULT-VALID"
    )
    assert decided["decided_by"]["principal_type"] == "controller"

    aad = json.loads((FIXTURES / "claim-grant-aad.json").read_text())
    digest = hashlib.sha256(canonical(aad["aad"]).encode()).hexdigest()
    assert digest == aad["canonical_aad_sha256"]
    for field in aad["mutate_each_field_must_fail"]:
        mutated = dict(aad["aad"])
        mutated[field] = f"mutated-{mutated[field]}"
        assert canonical(mutated) != canonical(aad["aad"])

    wire = json.loads(
        (FIXTURES / "claim-grant-wire-envelope.json").read_text()
    )
    assert canonical(wire["envelope"]["aad"]) == wire["aad_canonical_utf8"]
    assert "sha256:" + hashlib.sha256(
        wire["aad_canonical_utf8"].encode()
    ).hexdigest() == wire["aad_sha256"]
    assert set(wire["pre_open_mutations_must_fail"]) == {
        "profile_id",
        "kem",
        "kdf",
        "aead",
        "recipient_handoff_key_id",
        "encapsulated_key",
        "ciphertext",
        "aad",
    }
    assert wire["envelope"]["aad"]["owner_domain_id"] == "owner-domain_01"
    assert "business_owner_id" not in canonical(wire["envelope"])

    # The setup descriptor's field table lives in the SDK now. The generated
    # header has to carry a key constant for every field the vector declares,
    # because the serialiser writes the document out of those constants: a field
    # the contract gained and the header did not is a field this device cannot
    # advertise, and the controller then refuses a descriptor neither side
    # believes it got wrong.
    setup = json.loads((FIXTURES / "setup-descriptor.json").read_text())
    keys = re.search(r"struct SetupDescriptorKeys \{(.*?)\n\};", header, re.S)
    assert keys is not None
    declared = set(re.findall(r'"([a-z][a-z0-9_]*)"', keys.group(1)))
    assert declared == set(setup["required_fields"]) | set(setup["optional_fields"])
    # Two absences, each meaning something: no duration is an offer that does
    # not end, and no base identity is a device that has never been
    # commissioned — or one that was erased, which is now the same statement.
    assert setup["optional_fields"] == ["device_base_id", "expires_in_seconds"]
    # There is no constructor that could produce the sentinel this contract
    # used to ship, so the header must keep refusing it rather than clamping.
    assert "FromPositiveSeconds" in header
    assert "if (seconds < 1" in header
    for value in setup["trust_values"]:
        assert f'"{value}"' in header, value

    # The firmware's own identity test typed this vector out by hand: the same
    # digest, the same identity, the same evidence document, copied from the
    # contract and then unlinked from it. Rewording either side would have left
    # both green while they disagreed, so the copy is checked against the
    # vector it came from.
    vector = json.loads((FIXTURES / "commissioning-voucher.json").read_text())
    unit_test = (ROOT / "tests/device_instance_identity_test.cc").read_text()
    assert vector["device_instance_id"] == "device-instance-" + vector[
        "operational_spki_sha256"
    ].removeprefix("sha256:")
    for literal in (
        vector["operational_spki_sha256"].removeprefix("sha256:"),
        vector["operational_public_key"],
        vector["device_base_id"],
        vector["owner_domain_id"],
        vector["enrolled_base_key"]["nonce"],
        vector["evidence_canonical_utf8"].replace('"', '\\"'),
        vector["enrolled_base_key"]["canonical_utf8"].replace('"', '\\"'),
    ):
        assert literal in unit_test, literal
    # The base identity is the Hub's to mint. A device that could put its own
    # value here would be choosing the anchor its whole Claim history hangs
    # from, so the firmware must have no way to construct one.
    firmware = (ROOT / "main/eidolon/device_instance_identity.cc").read_text()
    assert "device-base-" not in firmware

    # The credential test reads the very bytes the Host signs.
    credential_test = (ROOT / "tests/commissioning_credential_test.cc").read_text()
    assert vector["voucher"]["compact"].replace("\n", "") in credential_test.replace(
        '"\n    "', ""
    ).replace('"', "")

    erase = json.loads((FIXTURES / "device-local-erase.json").read_text())
    assert canonical(erase["operation"]) == erase["operation_canonical_utf8"]
    assert canonical(erase["ack_signing_document"]) == erase["ack_canonical_utf8"]
    assert set(erase["operation"]["device_ref"]) == {
        "device_instance_id",
        "owner_domain_id",
        "owner_domain_generation",
        "claim_generation",
        "trust_epoch",
    }


if __name__ == "__main__":
    main()
