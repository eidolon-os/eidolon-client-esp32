#ifndef TEST_STUB_SSID_MANAGER_H_
#define TEST_STUB_SSID_MANAGER_H_

#include <string>
#include <vector>
#include <nvs.h>

struct SsidItem {
    std::string ssid;
    std::string password;
};

class SsidManager {
public:
    static SsidManager& GetInstance()
    {
        static SsidManager instance;
        return instance;
    }

    void AddSsid(const std::string& ssid, const std::string& password)
    {
        for (auto& item : ssids_) {
            if (item.ssid == ssid) {
                item.password = password;
                Save();
                return;
            }
        }
        ssids_.insert(ssids_.begin(), {ssid, password});
        Save();
    }

    void Clear() { ssids_.clear(); Save(); }
    void RestoreVolatileForTest(std::vector<SsidItem> saved) { ssids_ = std::move(saved); }
    void RemoveSsid(int index) { ssids_.erase(ssids_.begin() + index); Save(); }

    const std::vector<SsidItem>& GetSsidList() const { return ssids_; }

private:
    void Save() {
        nvs_handle_t handle = 0;
        if (nvs_open("wifi", NVS_READWRITE, &handle) != ESP_OK) return;
        for (int i = 0; i < 10; ++i) {
            const auto suffix = i == 0 ? std::string{} : std::to_string(i);
            if (i < static_cast<int>(ssids_.size())) {
                nvs_set_str(handle, ("ssid" + suffix).c_str(), ssids_[i].ssid.c_str());
                nvs_set_str(handle, ("password" + suffix).c_str(), ssids_[i].password.c_str());
            } else {
                nvs_erase_key(handle, ("ssid" + suffix).c_str());
                nvs_erase_key(handle, ("password" + suffix).c_str());
            }
        }
        nvs_commit(handle);
        nvs_close(handle);
    }
    std::vector<SsidItem> ssids_;
};

#endif
