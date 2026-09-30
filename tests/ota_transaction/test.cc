#include "ota_transaction.h"
#include <cassert>
#include <cstdio>
#include <vector>
static std::vector<int> calls;
static int failure;
esp_err_t esp_ota_begin(const esp_partition_t*, size_t, esp_ota_handle_t* h) { calls.push_back(1); *h=7; return failure==1?ESP_FAIL:ESP_OK; }
esp_err_t esp_ota_write(esp_ota_handle_t h, const void*, size_t) { assert(h==7); calls.push_back(2); return failure==2?ESP_FAIL:ESP_OK; }
esp_err_t esp_ota_end(esp_ota_handle_t h) { assert(h==7); calls.push_back(3); return failure==3?ESP_FAIL:ESP_OK; }
esp_err_t esp_ota_abort(esp_ota_handle_t h) { assert(h==7); calls.push_back(4); return ESP_OK; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t*) { calls.push_back(5); return failure==5?ESP_FAIL:ESP_OK; }
int main() {
    esp_partition_t part{1024};
    for (auto length : {size_t(0), size_t(1025)}) {
        OtaTransaction t(&part,length); assert(t.Begin()==ESP_ERR_INVALID_SIZE);
    }
    assert(calls.empty());
    for (int fail : {0,1,2,3,5}) {
        calls.clear(); failure=fail;
        {
            OtaTransaction t(&part,10);
            if (fail==1) { assert(t.Begin()==ESP_FAIL); }
            else {
                assert(t.Begin()==ESP_OK);
                assert(t.Begin()==ESP_ERR_INVALID_STATE);
                if (fail==2) assert(t.Write("12345",5)==ESP_FAIL);
                else {
                    assert(t.Write("12345",5)==ESP_OK);
                    assert(t.Write("12345",5)==ESP_OK);
                    assert(t.Finish()==(fail?ESP_FAIL:ESP_OK));
                    assert(t.Finish()==ESP_ERR_INVALID_STATE);
                }
            }
        }
        const std::vector<int> expected = fail==1 ? std::vector<int>{1} :
            fail==2 ? std::vector<int>{1,2,4} : fail==3 ? std::vector<int>{1,2,2,3} :
            std::vector<int>{1,2,2,3,5};
        assert(calls==expected);
    }
    failure=0;
    for (bool oversized : {false,true}) {
        calls.clear();
        {
            OtaTransaction t(&part,10); assert(t.Begin()==ESP_OK);
            assert(t.Write("12345",5)==ESP_OK);
            if (oversized) assert(t.Write("123456",6)==ESP_ERR_INVALID_SIZE);
            assert(t.Finish()==ESP_ERR_INVALID_SIZE);
        }
        assert((calls==std::vector<int>{1,2,4}));
    }
    // Transport failure/cancellation exits scope without finalizing.
    calls.clear(); { OtaTransaction t(&part,10); assert(t.Begin()==ESP_OK); }
    assert((calls==std::vector<int>{1,4}));
    puts("OTA transaction tests passed");
}
