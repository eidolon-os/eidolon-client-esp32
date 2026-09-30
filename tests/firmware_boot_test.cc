#include <cassert>
#include <iostream>
#include "eidolon/firmware_boot.h"

using namespace eidolon;
namespace {
bool pending, can_rollback, recovered, directory_ok;
esp_err_t rollback_result;
OwnerTrustLoadResult trust;
CommissioningIdentityLoad identity;
int confirmed, rolled_back, recovery_reads;
void Reset() {
    pending = can_rollback = recovered = directory_ok = true;
    rollback_result = ESP_OK;
    trust = OwnerTrustLoadResult::Loaded;
    identity = CommissioningIdentityLoad::Loaded;
    confirmed = rolled_back = recovery_reads = 0;
    boot_storage_error = ESP_OK;
}
}
const esp_partition_t* esp_ota_get_running_partition() {
    static esp_partition_t p{};
    return &p;
}
esp_err_t esp_ota_get_state_partition(const esp_partition_t*, esp_ota_img_states_t* state) {
    *state = pending ? ESP_OTA_IMG_PENDING_VERIFY : ESP_OTA_IMG_VALID;
    return ESP_OK;
}
bool esp_ota_check_rollback_is_possible() { return can_rollback; }
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot() { ++rolled_back; return rollback_result; }
esp_err_t esp_ota_mark_app_valid_cancel_rollback() { ++confirmed; return ESP_OK; }
namespace eidolon {
bool RecoverPendingCommissioningTransaction(bool*) { ++recovery_reads; return recovered; }
OwnerTrustLoadResult OwnerTrustStore::ReadActive(OwnerTrustBundle&) const { return trust; }
OwnerTrustStoreResult OwnerTrustStore::Stage(const OwnerTrustBundle&, uint32_t,
                                            const std::function<bool()>&) { assert(false); return {}; }
CommissioningIdentityLoad EspIdfCommissioningCredentialStore::LoadPrivateKey(std::string&) { return identity; }
struct DeviceAuthorityLocator::Impl {};
DeviceAuthorityLocator::DeviceAuthorityLocator() = default;
DeviceAuthorityLocator::~DeviceAuthorityLocator() = default;
DeviceAuthorityLocator& DeviceAuthorityLocator::GetInstance() { static DeviceAuthorityLocator l; return l; }
esp_err_t DeviceAuthorityLocator::ReloadCommissionedDirectory() { return directory_ok ? ESP_OK : ESP_FAIL; }
}
int main() {
    Reset(); pending = false;
    assert(ConfirmFirmwareBoot() == ESP_OK && confirmed == 0 && recovery_reads == 0);
    // No network port is involved: an offline healthy device confirms locally.
    Reset();
    assert(ConfirmFirmwareBoot() == ESP_OK && confirmed == 1 && rolled_back == 0);
    Reset(); trust = OwnerTrustLoadResult::NotFound; identity = CommissioningIdentityLoad::NotFound;
    assert(ConfirmFirmwareBoot() == ESP_OK && confirmed == 1);
    for (int failure = 0; failure < 6; ++failure) {
        for (bool fallback : {false, true}) {
            Reset(); can_rollback = fallback;
            if (failure == 0) recovered = false;
            if (failure == 1) trust = OwnerTrustLoadResult::Unavailable;
            if (failure == 2) identity = CommissioningIdentityLoad::Unavailable;
            if (failure == 3) identity = CommissioningIdentityLoad::NotFound;
            if (failure == 4) directory_ok = false;
            if (failure == 5) boot_storage_error = ESP_FAIL;
            assert(ConfirmFirmwareBoot() != ESP_OK);
            assert(rolled_back == (fallback ? 1 : 0));
            assert(confirmed == (fallback ? 0 : 1)); // only-app recovery stays bootable
        }
    }
    Reset(); rollback_result = ESP_FAIL;
    assert(RecoverFirmwareBoot() == ESP_OK && rolled_back == 1 && confirmed == 1);
    Reset(); pending = false;
    assert(RecoverFirmwareBoot() == ESP_OK && rolled_back == 0 && confirmed == 0);
    std::cout << "firmware local confirmation, rollback and only-app recovery: PASS\n";
}
