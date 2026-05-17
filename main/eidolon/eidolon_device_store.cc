#include "eidolon_device_store.h"

#include "settings.h"

#include <esp_log.h>

#define TAG "EidolonDeviceStore"

namespace eidolon {

bool EidolonDeviceStore::LoadMicEnabled(bool default_enabled) const
{
    Settings settings(kEidolonDeviceNvsNamespace, false);
    return settings.GetBool("mic_enabled", default_enabled);
}

esp_err_t EidolonDeviceStore::SaveMicEnabled(bool enabled)
{
    Settings settings(kEidolonDeviceNvsNamespace, true);
    settings.SetBool("mic_enabled", enabled);
    ESP_LOGI(TAG, "Saved mic_enabled=%d", enabled ? 1 : 0);
    return ESP_OK;
}

bool EidolonDeviceStore::LoadThemeApplied() const
{
    Settings settings(kEidolonDeviceNvsNamespace, false);
    return settings.GetBool("theme_applied", false);
}

esp_err_t EidolonDeviceStore::MarkThemeApplied()
{
    Settings settings(kEidolonDeviceNvsNamespace, true);
    settings.SetBool("theme_applied", true);
    return ESP_OK;
}

}  // namespace eidolon
