#ifndef EIDOLON_DEVICE_STORE_H_
#define EIDOLON_DEVICE_STORE_H_

#include <esp_err.h>

namespace eidolon {

inline constexpr const char* kEidolonDeviceNvsNamespace = "eidolon_device";

class EidolonDeviceStore {
public:
    bool LoadMicEnabled(bool default_enabled = true) const;
    esp_err_t SaveMicEnabled(bool enabled);

    bool LoadThemeApplied() const;
    esp_err_t MarkThemeApplied();
};

}  // namespace eidolon

#endif  // EIDOLON_DEVICE_STORE_H_
