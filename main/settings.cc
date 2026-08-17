#include "settings.h"

#include <esp_log.h>
#include <nvs_flash.h>

#define TAG "Settings"

Settings::Settings(const std::string& ns, bool read_write) : ns_(ns), read_write_(read_write) {
    nvs_open(ns.c_str(), read_write_ ? NVS_READWRITE : NVS_READONLY, &nvs_handle_);
}

Settings::~Settings() {
    if (nvs_handle_ != 0) {
        if (read_write_ && dirty_) {
            Commit();
        }
        nvs_close(nvs_handle_);
    }
}

esp_err_t Settings::Commit() {
    if (nvs_handle_ == 0 || !read_write_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!dirty_) {
        return ESP_OK;
    }
    esp_err_t err = nvs_commit(nvs_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit(%s) failed: %s", ns_.c_str(), esp_err_to_name(err));
        return err;
    }
    dirty_ = false;
    return ESP_OK;
}

std::string Settings::GetString(const std::string& key, const std::string& default_value) {
    if (nvs_handle_ == 0) {
        return default_value;
    }

    size_t length = 0;
    if (nvs_get_str(nvs_handle_, key.c_str(), nullptr, &length) != ESP_OK) {
        return default_value;
    }

    std::string value;
    value.resize(length);
    if (nvs_get_str(nvs_handle_, key.c_str(), value.data(), &length) != ESP_OK) {
        return default_value;
    }
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

esp_err_t Settings::SetString(const std::string& key, const std::string& value) {
    if (!read_write_ || nvs_handle_ == 0) {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
        return ESP_ERR_INVALID_STATE;
    }
    // Dedup: a config refresh usually re-saves an identical blob. Skipping the
    // unchanged write spares NVS space and flash wear (and avoids needlessly
    // filling the partition).
    size_t existing_len = 0;
    if (nvs_get_str(nvs_handle_, key.c_str(), nullptr, &existing_len) == ESP_OK) {
        std::string current(existing_len, '\0');
        if (nvs_get_str(nvs_handle_, key.c_str(), current.data(), &existing_len) == ESP_OK) {
            while (!current.empty() && current.back() == '\0') {
                current.pop_back();
            }
            if (current == value) {
                return ESP_OK;
            }
        }
    }
    // Degrade instead of aborting: a failed NVS write (e.g. partition full) must
    // not crash the boot. Aborting here was the start of the "NVS full -> crash
    // loop -> wipe NVS -> lose P-256 identity" chain. The in-RAM value still works
    // this session; the next boot falls back to the last good stored value.
    esp_err_t err = nvs_set_str(nvs_handle_, key.c_str(), value.c_str());
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        // Replacing a string writes the new value before the old one is dropped,
        // so on a nearly full partition an entry cannot be replaced even when the
        // space it needs is held by the very entry it would replace. Dropping
        // ours first is safe in a way that wiping the partition is not: what is
        // discarded is this key's previous value, which costs a re-fetch, rather
        // than the identity that lives beside it and cannot be re-fetched at all.
        ESP_LOGW(TAG, "%s/%s does not fit; replacing it in place", ns_.c_str(), key.c_str());
        if (nvs_erase_key(nvs_handle_, key.c_str()) == ESP_OK) {
            nvs_commit(nvs_handle_);
            err = nvs_set_str(nvs_handle_, key.c_str(), value.c_str());
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str(%s/%s) failed: %s", ns_.c_str(), key.c_str(),
                 esp_err_to_name(err));
        return err;
    }
    dirty_ = true;
    return ESP_OK;
}

int32_t Settings::GetInt(const std::string& key, int32_t default_value) {
    if (nvs_handle_ == 0) {
        return default_value;
    }

    int32_t value;
    if (nvs_get_i32(nvs_handle_, key.c_str(), &value) != ESP_OK) {
        return default_value;
    }
    return value;
}

void Settings::SetInt(const std::string& key, int32_t value) {
    if (!read_write_) {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
        return;
    }
    int32_t existing;
    if (nvs_get_i32(nvs_handle_, key.c_str(), &existing) == ESP_OK && existing == value) {
        return;  // unchanged
    }
    esp_err_t err = nvs_set_i32(nvs_handle_, key.c_str(), value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_i32(%s/%s) failed: %s", ns_.c_str(), key.c_str(),
                 esp_err_to_name(err));
        return;
    }
    dirty_ = true;
}

bool Settings::GetBool(const std::string& key, bool default_value) {
    if (nvs_handle_ == 0) {
        return default_value;
    }

    uint8_t value;
    if (nvs_get_u8(nvs_handle_, key.c_str(), &value) != ESP_OK) {
        return default_value;
    }
    return value != 0;
}

void Settings::SetBool(const std::string& key, bool value) {
    if (!read_write_) {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
        return;
    }
    uint8_t existing;
    if (nvs_get_u8(nvs_handle_, key.c_str(), &existing) == ESP_OK && (existing != 0) == value) {
        return;  // unchanged
    }
    esp_err_t err = nvs_set_u8(nvs_handle_, key.c_str(), value ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u8(%s/%s) failed: %s", ns_.c_str(), key.c_str(),
                 esp_err_to_name(err));
        return;
    }
    dirty_ = true;
}

void Settings::EraseKey(const std::string& key) {
    if (read_write_) {
        auto ret = nvs_erase_key(nvs_handle_, key.c_str());
        if (ret != ESP_ERR_NVS_NOT_FOUND) {
            ESP_ERROR_CHECK(ret);
            dirty_ = true;
        }
    } else {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}

void Settings::EraseAll() {
    if (read_write_) {
        ESP_ERROR_CHECK(nvs_erase_all(nvs_handle_));
    } else {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}
