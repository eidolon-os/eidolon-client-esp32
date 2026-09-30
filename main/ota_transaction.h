#pragma once

#include <esp_ota_ops.h>
#include <cstddef>

// Own exactly one IDF OTA transaction. IDF retains responsibility for image
// validation, flash writes and boot metadata. Never select an incomplete image.
class OtaTransaction {
public:
    OtaTransaction(const esp_partition_t* partition, size_t expected)
        : partition_(partition), expected_(expected) {}
    ~OtaTransaction() { if (active_) esp_ota_abort(handle_); }
    OtaTransaction(const OtaTransaction&) = delete;
    OtaTransaction& operator=(const OtaTransaction&) = delete;

    esp_err_t Begin() {
        if (active_ || finished_) return ESP_ERR_INVALID_STATE;
        if (!partition_ || expected_ == 0 || expected_ > partition_->size)
            return ESP_ERR_INVALID_SIZE;
        const auto err = esp_ota_begin(partition_, OTA_WITH_SEQUENTIAL_WRITES, &handle_);
        active_ = err == ESP_OK;
        return err;
    }
    esp_err_t Write(const void* data, size_t size) {
        if (!active_) return ESP_ERR_INVALID_STATE;
        if (size > expected_ - written_) return ESP_ERR_INVALID_SIZE;
        const auto err = esp_ota_write(handle_, data, size);
        if (err == ESP_OK) written_ += size;
        return err;
    }
    esp_err_t Finish() {
        if (!active_) return ESP_ERR_INVALID_STATE;
        if (written_ != expected_) return ESP_ERR_INVALID_SIZE;
        // esp_ota_end consumes the handle even when image validation fails.
        active_ = false;
        finished_ = true;
        const auto err = esp_ota_end(handle_);
        if (err != ESP_OK) return err;
        return esp_ota_set_boot_partition(partition_);
    }
private:
    const esp_partition_t* partition_;
    size_t expected_;
    size_t written_ = 0;
    esp_ota_handle_t handle_ = 0;
    bool active_ = false;
    bool finished_ = false;
};
