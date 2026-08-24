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
