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
    setup_text = (ROOT / "main/boards/common/wifi_board.cc").read_text()

    assert "owner_trust,data, nvs" in partition_text
    for preserved in ("phy_init", "otadata", "ota_0", "ota_1", "assets"):
        assert preserved in partition_text
        assert f'"{preserved}"' in policy_text

    # B3/R23: the base identity and the operational key it was issued to are
    # one identity, kept in one namespace, and both operations that end that
    # identity must end all of it. Naming only the private key left a Body
    # holding a lineage it could no longer demonstrate, and the Authority can
    # answer such a Body nothing but 401, forever. Neither eraser may restate
    # the key names: they come from the store that owns them, so a key added
    # there is erased by both without either being edited.
    credential_header = (
        ROOT / "main/eidolon/esp_idf_commissioning_credential_store.h"
    ).read_text()
    recovery_text = (ROOT / "main/eidolon/device_physical_recovery.cc").read_text()
    for key in ('"base_id"', '"voucher"', '"voucher_jti"', '"voucher_exp"'):
        assert key in credential_header, key
        for eraser, name in (
            (policy_text, "the Owner's remote erase"),
            (recovery_text, "physical recovery"),
        ):
            assert key not in eraser, f"{name} restates {key} instead of using the store's"
    for eraser in (policy_text, recovery_text):
        assert "kCommissioningCredentialKeys" in eraser
        assert "kCommissioningCredentialNamespace" in eraser
        assert '"eidolon_id"' not in eraser
        assert '"p256_priv"' in eraser

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

    # D8 / §1 item 10: the window-opening decision consults the RemovalJournal
    # BEFORE it asks for a window, not afterwards through the Claim that fails.
    # Both automatic doors into setup — a boot with no network profile, and a
    # connect timeout — go through StartWifiConfigMode, so the order of these
    # two lines is the whole guarantee.
    blocks = setup_text.index("RemovalBlocksCommissioning()")
    request = setup_text.index("CommissioningRuntime::GetInstance().RequestOpen()")
    assert blocks < request
    assert 'kEnrollmentJournalKey = "enrollment"' in claim_store_text
    assert 'kActiveClaimKey = "active_claim"' in claim_store_text
    assert '"onboarding"' not in claim_store_text


if __name__ == "__main__":
    main()
