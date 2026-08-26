#ifndef TEST_STUB_SSID_MANAGER_H_
#define TEST_STUB_SSID_MANAGER_H_

#include <string>
#include <vector>

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
                return;
            }
        }
        ssids_.push_back({ssid, password});
    }

    void Clear() { ssids_.clear(); }

    const std::vector<SsidItem>& GetSsidList() const { return ssids_; }

private:
    std::vector<SsidItem> ssids_;
};

#endif
