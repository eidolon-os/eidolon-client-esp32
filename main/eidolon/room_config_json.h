#ifndef EIDOLON_ROOM_CONFIG_JSON_H_
#define EIDOLON_ROOM_CONFIG_JSON_H_

#include <algorithm>
#include <cJSON.h>
#include "hub_types.h"

namespace eidolon {
// Optional additive v2 field. Old bindings remain a single route.
inline bool ReadRoomServerUrls(const cJSON* room, RoomConfig& out)
{
    out.server_urls.clear();
    const cJSON* values = cJSON_GetObjectItemCaseSensitive(room, "server_urls");
    if (values == nullptr) return true;
    if (!cJSON_IsArray(values) || cJSON_GetArraySize(values) == 0) return false;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, values) {
        if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0]) return false;
        const std::string value(item->valuestring);
        if (std::find(out.server_urls.begin(), out.server_urls.end(), value) != out.server_urls.end()) return false;
        out.server_urls.push_back(value);
    }
    return out.server_urls.front() == out.server_url;
}
}
#endif
