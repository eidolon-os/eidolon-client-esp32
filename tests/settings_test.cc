#include "settings.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <variant>

namespace {
constexpr esp_err_t kNoSpace = 0x1105;
using Value = std::variant<std::string, int32_t, uint8_t>;
std::map<std::string, Value> disk;
esp_err_t write_result = ESP_OK, erase_result = ESP_OK, commit_result = ESP_OK;
int writes = 0, erases = 0, commits = 0;
void Reset() {
    disk = {{"text", std::string("last-good")}, {"number", int32_t(10)}, {"flag", uint8_t(1)}};
    write_result = erase_result = commit_result = ESP_OK;
    writes = erases = commits = 0;
}
template<class T> esp_err_t Get(const char* key, T& value) {
    const auto it = disk.find(key);
    if (it == disk.end()) return ESP_ERR_NVS_NOT_FOUND;
    const auto* saved = std::get_if<T>(&it->second);
    if (!saved) return ESP_FAIL;
    value = *saved;
    return ESP_OK;
}
template<class T> esp_err_t Set(const char* key, T value) {
    ++writes;
    if (write_result != ESP_OK) return write_result;
    // IDF writes may already be durable before Commit. Settings does not
    // promise a multi-key transaction or undo earlier successful setters.
    disk[key] = value;
    return ESP_OK;
}
}
const char* esp_err_to_name(esp_err_t) { return "injected error"; }
esp_err_t nvs_open(const char*, int, nvs_handle_t* h) { *h = 1; return ESP_OK; }
void nvs_close(nvs_handle_t) {}
esp_err_t nvs_get_str(nvs_handle_t, const char* key, char* out, size_t* size) {
    std::string value;
    const auto err = Get(key, value);
    if (err != ESP_OK) return err;
    if (out) { assert(*size >= value.size() + 1); std::memcpy(out, value.c_str(), value.size() + 1); }
    *size = value.size() + 1;
    return ESP_OK;
}
esp_err_t nvs_get_i32(nvs_handle_t, const char* key, int32_t* out) { return Get(key, *out); }
esp_err_t nvs_get_u8(nvs_handle_t, const char* key, uint8_t* out) { return Get(key, *out); }
esp_err_t nvs_set_str(nvs_handle_t, const char* key, const char* value) { return Set(key, std::string(value)); }
esp_err_t nvs_set_i32(nvs_handle_t, const char* key, int32_t value) { return Set(key, value); }
esp_err_t nvs_set_u8(nvs_handle_t, const char* key, uint8_t value) { return Set(key, value); }
esp_err_t nvs_erase_key(nvs_handle_t, const char* key) {
    ++erases;
    if (erase_result != ESP_OK) return erase_result;
    return disk.erase(key) ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_erase_all(nvs_handle_t) {
    ++erases;
    if (erase_result != ESP_OK) return erase_result;
    disk.clear(); return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t) { ++commits; return commit_result; }
int main() {
    for (int type = 0; type < 3; ++type) {
        Reset();
        Settings settings("test", true);
        assert(settings.SetString("other", "successful write") == ESP_OK);
        const auto before = disk;
        write_result = kNoSpace;
        if (type == 0) assert(settings.SetString("text", "replacement") == kNoSpace);
        if (type == 1) settings.SetInt("number", 20);
        if (type == 2) settings.SetBool("flag", false);
        assert(settings.Commit() == kNoSpace && disk == before && erases == 0);
        // Later successful/deduplicated writes cannot hide this object's error.
        write_result = ESP_OK;
        assert(settings.SetString("other", "successful write") == ESP_OK);
        assert(settings.Commit() == kNoSpace);
    }
    for (bool all : {false, true}) {
        Reset();
        Settings settings("test", true);
        const auto before = disk;
        erase_result = ESP_FAIL;
        if (all) settings.EraseAll(); else settings.EraseKey("text");
        assert(settings.Commit() == ESP_FAIL && disk == before);
    }
    Reset();
    {
        Settings settings("test", true);
        settings.EraseKey("absent");
        assert(settings.Commit() == ESP_OK && commits == 0);
        assert(settings.SetString("text", "last-good") == ESP_OK && writes == 0);
        settings.SetInt("number", 20);
        commit_result = ESP_FAIL;
        assert(settings.Commit() == ESP_FAIL);
        commit_result = ESP_OK;
        assert(settings.Commit() == ESP_OK);
        const auto committed = commits;
        assert(settings.Commit() == ESP_OK && commits == committed);
    }
    Reset();
    { Settings settings("test", true); settings.SetBool("flag", false); }
    assert(commits == 1 && std::get<uint8_t>(disk.at("flag")) == 0);
    std::cout << "Settings preserves old values and propagates write/erase/commit failures: PASS\n";
}
