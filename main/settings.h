#ifndef SETTINGS_H
#define SETTINGS_H

#include <string>
#include <nvs_flash.h>

class Settings {
public:
    Settings(const std::string& ns, bool read_write = false);
    ~Settings();

    std::string GetString(const std::string& key, const std::string& default_value = "");
    esp_err_t SetString(const std::string& key, const std::string& value);
    // Reports any setter/erase failure during this object's lifetime. NVS
    // setters may already be durable; this is not a multi-key transaction.
    esp_err_t Commit();
    int32_t GetInt(const std::string& key, int32_t default_value = 0);
    void SetInt(const std::string& key, int32_t value);
    bool GetBool(const std::string& key, bool default_value = false);
    void SetBool(const std::string& key, bool value);
    void EraseKey(const std::string& key);
    void EraseAll();

private:
    std::string ns_;
    nvs_handle_t nvs_handle_ = 0;
    bool read_write_ = false;
    bool dirty_ = false;
    esp_err_t write_error_ = ESP_OK;
};

#endif
