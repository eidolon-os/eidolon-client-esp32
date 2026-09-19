#include "eidolon/hub_trust_store.h"
#include <nvs.h>
#include <esp_partition.h>
#include <cassert>
#include <iostream>
const char* esp_err_to_name(esp_err_t) { return "test error"; }
namespace {
bool dedicated = false;
esp_err_t legacy_open_result = ESP_ERR_NVS_NOT_FOUND;
esp_err_t open_result = ESP_ERR_NVS_NOT_FOUND;
esp_err_t marker_result = ESP_ERR_NVS_NOT_FOUND;
}
const esp_partition_t* esp_partition_find_first(int, int, const char*) {
    static esp_partition_t partition;
    return dedicated ? &partition : nullptr;
}
esp_err_t nvs_open(const char*, int mode, nvs_handle_t* h) {
    assert(mode == NVS_READONLY); // Empty reads must never create namespaces.
    *h = 1;
    return dedicated ? legacy_open_result : open_result;
}
esp_err_t nvs_open_from_partition(const char*, const char*, int mode, nvs_handle_t* h) {
    assert(dedicated && mode == NVS_READONLY);
    *h = 2;
    return open_result;
}
void nvs_close(nvs_handle_t) {}
esp_err_t nvs_get_str(nvs_handle_t, const char*, char*, size_t* size) {
    if (marker_result==ESP_OK) *size=0; // malformed marker is never treated as empty
    return marker_result;
}
esp_err_t nvs_get_u32(nvs_handle_t, const char*, uint32_t*) { return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_set_str(nvs_handle_t, const char*, const char*) { return ESP_FAIL; }
esp_err_t nvs_set_u32(nvs_handle_t, const char*, uint32_t) { return ESP_FAIL; }
esp_err_t nvs_commit(nvs_handle_t) { return ESP_FAIL; }
esp_err_t nvs_erase_key(nvs_handle_t, const char*) { return ESP_FAIL; }
namespace eidolon {
bool IsCommissionableCertificate(const std::string&) { assert(false); return false; }
}
int main() {
    eidolon::OwnerTrustStore store;
    eidolon::OwnerTrustBundle bundle;
    using Result = eidolon::OwnerTrustLoadResult;
    // Both physical layouts must expose exactly the same empty/error contract.
    for (bool layout : {false, true}) {
        dedicated = layout;
        open_result = ESP_ERR_NVS_NOT_FOUND;
        marker_result = ESP_ERR_NVS_NOT_FOUND;
        assert(store.ReadActive(bundle) == Result::NotFound);
        open_result = ESP_FAIL;
        assert(store.ReadActive(bundle) == Result::Unavailable);
        open_result = ESP_OK;
        assert(store.ReadActive(bundle) == Result::NotFound);
        marker_result = ESP_FAIL;
        assert(store.ReadActive(bundle) == Result::Unavailable);
        marker_result = ESP_OK;
        assert(store.ReadActive(bundle) == Result::Unavailable);
    }
    // An empty dedicated partition must not hide an unreadable legacy source.
    open_result = ESP_ERR_NVS_NOT_FOUND;
    legacy_open_result = ESP_FAIL;
    assert(store.ReadActive(bundle) == Result::Unavailable);
    std::cout << "Fresh Owner trust storage layouts: PASS\n";
}
