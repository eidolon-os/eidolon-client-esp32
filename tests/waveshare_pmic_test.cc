#include "boards/waveshare/esp32-s3-touch-amoled-2.06/pmic.h"
#include <array>
#include <cassert>
#include <cstdio>

namespace {
std::array<uint8_t, 256> registers;
int attempt, fail_at, writes_after_failure, releases;
bool failed, required_rail_dropped;
void Reset(int failure) {
    registers.fill(0);
    registers[0x90] = 0x03;
    attempt = writes_after_failure = releases = 0;
    fail_at = failure;
    failed = required_rail_dropped = false;
}
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t, const uint8_t* bytes, size_t size, int) {
    assert(size == 2);
    if (failed) ++writes_after_failure;
    if (++attempt == fail_at) { failed = true; return ESP_FAIL; }
    registers[bytes[0]] = bytes[1];
    // A cut after ANY successful write must leave the audio/control rails up.
    if ((registers[0x90] & 0x03) != 0x03) required_rail_dropped = true;
    return ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t) { ++releases; return ESP_OK; }
int main() {
    int operation_count;
    Reset(-1);
    { Pmic device(nullptr, 0x34); assert(device.InitializationError() == ESP_OK); operation_count = attempt; }
    assert(!required_rail_dropped && releases == 1);
    for (int cut = 1; cut <= operation_count; ++cut) {
        Reset(cut);
        { Pmic device(nullptr, 0x34); assert(device.InitializationError() == ESP_FAIL); }
        assert(!required_rail_dropped && writes_after_failure == 0 && releases == 1);
    }
    std::printf("waveshare_pmic_test: PASS (%d write failure boundaries)\n", operation_count);
}
