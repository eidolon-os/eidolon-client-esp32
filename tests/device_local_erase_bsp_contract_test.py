from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    partition_text = (ROOT / "partitions/v2/16m_eidolon_box3.csv").read_text()
    storage_text = (
        ROOT / "main/eidolon/esp_idf_owner_data_erase_storage.cc"
    ).read_text()
    policy_text = (
        ROOT / "main/eidolon/esp_idf_device_local_erase_adapter.cc"
    ).read_text()
    cmake_text = (ROOT / "main/CMakeLists.txt").read_text()
    boot_text = (ROOT / "main/eidolon/hub_activator.cc").read_text()
    claim_store_text = (ROOT / "main/eidolon/hub_config_store.cc").read_text()

    assert "owner_trust,data, nvs" in partition_text
    for preserved in ("phy_init", "otadata", "ota_0", "ota_1", "assets"):
        assert preserved in partition_text
        assert f'"{preserved}"' in policy_text

    assert "nvs_flash_erase" not in storage_text
    assert "erase_flash" not in storage_text
    assert "esp_partition_erase_range" in storage_text
    assert "nvs_set_blob" in storage_text
    for slot in ('"ap0"', '"ap1"', '"cj0"', '"cj1"'):
        assert slot in storage_text
    assert '"face_db"' in policy_text
    assert '"owner_trust"' in policy_text
    assert '"eid_erase"' in policy_text
    assert '"eidolon/esp_idf_device_local_erase_adapter.cc"' in cmake_text
    assert '"eidolon/esp_idf_owner_data_erase_storage.cc"' in cmake_text
    assert '"eidolon/device_boot_recovery.cc"' in cmake_text
    resume = boot_text.index("DeviceBootRecovery::ResumePendingRemoval()")
    discovery = boot_text.index("HubDiscovery discovery")
    assert resume < discovery
    assert "AllowsClaimOrRuntime" in boot_text
    assert 'kEnrollmentJournalKey = "enrollment"' in claim_store_text
    assert 'kActiveClaimKey = "active_claim"' in claim_store_text
    assert '"onboarding"' not in claim_store_text


if __name__ == "__main__":
    main()
