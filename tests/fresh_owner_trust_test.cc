#include "eidolon/hub_trust_store.h"
#include <nvs.h>
#include <esp_partition.h>
#include <cassert>
#include <iostream>
const char* esp_err_to_name(esp_err_t) { return "test error"; }
namespace {
esp_err_t open_result = ESP_ERR_NVS_NOT_FOUND;
esp_err_t marker_result = ESP_ERR_NVS_NOT_FOUND;
}
const esp_partition_t* esp_partition_find_first(int, int, const char*) { return nullptr; }
esp_err_t nvs_open(const char*, int, nvs_handle_t* h) { *h=1; return open_result; }
esp_err_t nvs_open_from_partition(const char*, const char*, int, nvs_handle_t*) { return ESP_FAIL; }
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
    // Factory-erased boards with no owner_trust partition and no namespace.
    assert(store.ReadActive(bundle)==Result::NotFound);
    open_result=ESP_FAIL;
    assert(store.ReadActive(bundle)==Result::Unavailable);
    open_result=ESP_OK;
    assert(store.ReadActive(bundle)==Result::NotFound);
    marker_result=ESP_FAIL;
    assert(store.ReadActive(bundle)==Result::Unavailable);
    marker_result=ESP_OK;
    assert(store.ReadActive(bundle)==Result::Unavailable);
    std::cout << "Fresh Owner trust namespace: PASS\n";
}
