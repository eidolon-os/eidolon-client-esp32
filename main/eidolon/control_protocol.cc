#include "control_protocol.h"

#include <cJSON.h>
#include <esp_log.h>

#include <ctime>

#define TAG "ControlProtocol"

namespace eidolon {

namespace {

std::string JsonString(const cJSON* item)
{
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

std::string PrintJson(const cJSON* item)
{
    if (!item) {
        return "{}";
    }
    char* raw = cJSON_PrintUnformatted(item);
    if (!raw) {
        return "{}";
    }
    std::string out(raw);
    cJSON_free(raw);
    return out;
}

std::string FirstString(const cJSON* root, const char* a, const char* b = nullptr, const char* c = nullptr)
{
    std::string value = JsonString(cJSON_GetObjectItem(root, a));
    if (!value.empty() || !b) {
        return value;
    }
    value = JsonString(cJSON_GetObjectItem(root, b));
    if (!value.empty() || !c) {
        return value;
    }
    return JsonString(cJSON_GetObjectItem(root, c));
}

bool IsExpired(const cJSON* ts_item, const cJSON* ttl_item)
{
    if (!cJSON_IsNumber(ts_item) || !cJSON_IsNumber(ttl_item)) {
        return false;
    }
    const auto ts_ms = static_cast<long long>(ts_item->valuedouble);
    const auto ttl_ms = static_cast<long long>(ttl_item->valuedouble);
    if (ts_ms <= 0 || ttl_ms <= 0) {
        return false;
    }

    const std::time_t now_seconds = std::time(nullptr);
    if (now_seconds < 1700000000) {
        return false;
    }
    const long long now_ms = static_cast<long long>(now_seconds) * 1000;
    return now_ms > ts_ms + ttl_ms;
}

void AddString(cJSON* root, const char* key, const std::string& value)
{
    if (!value.empty()) {
        cJSON_AddStringToObject(root, key, value.c_str());
    }
}

}  // namespace

ControlCommand ParseControlCommand(const std::string& json)
{
    ControlCommand command;
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root || !cJSON_IsObject(root)) {
        ESP_LOGW(TAG, "Malformed control JSON");
        cJSON_Delete(root);
        return command;
    }

    const cJSON* version = cJSON_GetObjectItem(root, "v");
    const std::string kind = JsonString(cJSON_GetObjectItem(root, "kind"));
    if (cJSON_IsNumber(version) && version->valueint == kControlProtocolVersion && kind == "cmd") {
        command.is_v1 = true;
        command.id = FirstString(root, "id", "command_id");
        command.op = FirstString(root, "op", "type", "command");
        command.payload = PrintJson(cJSON_GetObjectItem(root, "payload"));
        command.expired = IsExpired(cJSON_GetObjectItem(root, "ts"), cJSON_GetObjectItem(root, "ttl_ms"));
    } else {
        const cJSON* payload = cJSON_GetObjectItem(root, "payload");
        if (cJSON_IsObject(payload)) {
            command.op = FirstString(payload, "op", "type", "command");
        }
        if (command.op.empty()) {
            command.op = FirstString(root, "op", "type", "command");
        }
        command.id = FirstString(root, "id", "command_id");
        command.payload = cJSON_IsObject(payload) ? PrintJson(payload) : PrintJson(root);
    }

    command.valid = !command.op.empty();
    cJSON_Delete(root);
    return command;
}

std::string BuildControlAck(const ControlCommand& command,
                            const std::string& device_id,
                            const std::string& status,
                            const std::string& code,
                            const std::string& message,
                            const std::string& result_json)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", kControlProtocolVersion);
    cJSON_AddStringToObject(root, "kind", result_json.empty() ? "ack" : "result");
    AddString(root, "id", command.id.empty() ? "" : "ack-" + command.id);
    AddString(root, "ref", command.id);
    AddString(root, "device_id", device_id);
    AddString(root, "op", command.op);
    cJSON_AddStringToObject(root, "status", status.c_str());
    cJSON_AddStringToObject(root, "code", code.c_str());
    AddString(root, "message", message);
    cJSON_AddNumberToObject(root, "ts", static_cast<double>(std::time(nullptr)) * 1000);

    if (!result_json.empty()) {
        cJSON* result = cJSON_Parse(result_json.c_str());
        if (result) {
            cJSON_AddItemToObject(root, "result", result);
        }
    }

    char* raw = cJSON_PrintUnformatted(root);
    std::string out = raw ? raw : "{}";
    if (raw) {
        cJSON_free(raw);
    }
    cJSON_Delete(root);
    return out;
}

}  // namespace eidolon
