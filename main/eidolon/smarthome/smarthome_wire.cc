#include "smarthome_wire.h"

#include <cJSON.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>

namespace eidolon::smarthome {

namespace {

// The SDK caps a snapshot at 64 KiB and a voice result at 3072 bytes, measured
// as compact JSON. The payload handed to us was re-printed by the control
// parser, whose number formatting can differ by a few bytes, so the local caps
// only reject what no conforming producer could send.
constexpr size_t kMaxStatePayloadBytes = 96 * 1024;
constexpr size_t kMaxResultPayloadBytes = 4096;
constexpr double kMaxSafeInteger = 9007199254740991.0;

using JsonPtr = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

struct Failure {
    std::string* error;
    bool operator()(const char* what) const {
        if (error != nullptr) *error = what;
        return false;
    }
};

const cJSON* Field(const cJSON* object, const char* key) {
    return cJSON_GetObjectItemCaseSensitive(object, key);
}

size_t CodePoints(const std::string& text) {
    size_t count = 0;
    for (unsigned char c : text) count += (c & 0xC0) != 0x80;
    return count;
}

bool Blank(const std::string& text) {
    for (unsigned char c : text) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
    }
    return true;
}

bool ReadString(const cJSON* item, std::string& out) {
    if (!cJSON_IsString(item) || item->valuestring == nullptr) return false;
    out = item->valuestring;
    return true;
}

bool ReadIdentifier(const cJSON* item, std::string& out) {
    return ReadString(item, out) && IsIdentifier(out);
}

// SDK Name: 1..32 characters with at least one non-space.
bool ReadName(const cJSON* item, std::string& out) {
    return ReadString(item, out) && !out.empty() && !Blank(out) && CodePoints(out) <= 32;
}

bool IsIntegral(const cJSON* item) {
    return cJSON_IsNumber(item) && std::isfinite(item->valuedouble) &&
           std::floor(item->valuedouble) == item->valuedouble;
}

bool ReadRevision(const cJSON* item, uint64_t& out) {
    if (!IsIntegral(item) || item->valuedouble < 0 || item->valuedouble > kMaxSafeInteger) return false;
    out = static_cast<uint64_t>(item->valuedouble);
    return true;
}

bool ReadInt(const cJSON* item, int low, int high, int& out) {
    if (!IsIntegral(item) || item->valuedouble < low || item->valuedouble > high) return false;
    out = static_cast<int>(item->valuedouble);
    return true;
}

bool ReadNumber(const cJSON* item, double& out) {
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble)) return false;
    out = item->valuedouble;
    return true;
}

bool SchemaVersionOk(const cJSON* payload) {
    const cJSON* version = Field(payload, "schema_version");
    return version == nullptr || (IsIntegral(version) && version->valuedouble == kSchemaVersion);
}

bool ParseDeviceType(const std::string& text, DeviceType& out) {
    static constexpr struct { const char* name; DeviceType type; } kTypes[] = {
        {"light", DeviceType::Light},         {"switch", DeviceType::Switch},
        {"climate", DeviceType::Climate},     {"water_heater", DeviceType::WaterHeater},
        {"cover", DeviceType::Cover},         {"fan", DeviceType::Fan},
        {"media", DeviceType::Media},         {"appliance", DeviceType::Appliance},
        {"lock", DeviceType::Lock},           {"camera", DeviceType::Camera},
        {"sensor", DeviceType::Sensor},
    };
    for (const auto& entry : kTypes) {
        if (text == entry.name) {
            out = entry.type;
            return true;
        }
    }
    return false;
}

ThermostatMode ParseMode(const std::string& text) {
    if (text == "cool") return ThermostatMode::Cool;
    if (text == "heat") return ThermostatMode::Heat;
    if (text == "auto") return ThermostatMode::Auto;
    if (text == "fan") return ThermostatMode::Fan;
    if (text == "dry") return ThermostatMode::Dry;
    return ThermostatMode::Unknown;
}

RunState ParseRunState(const std::string& text) {
    if (text == "idle") return RunState::Idle;
    if (text == "running") return RunState::Running;
    if (text == "paused") return RunState::Paused;
    if (text == "docked") return RunState::Docked;
    return RunState::Unknown;
}

bool ParseOrigin(const std::string& text, OriginKind& out) {
    if (text == "voice") out = OriginKind::Voice;
    else if (text == "text") out = OriginKind::Text;
    else if (text == "touch") out = OriginKind::Touch;
    else if (text == "scene") out = OriginKind::Scene;
    else if (text == "mobile") out = OriginKind::Mobile;
    else if (text == "automation") out = OriginKind::Automation;
    else return false;
    return true;
}

bool ParseOutcome(const std::string& text, VoiceOutcome& out) {
    if (text == "executed") out = VoiceOutcome::Executed;
    else if (text == "partial") out = VoiceOutcome::Partial;
    else if (text == "answered") out = VoiceOutcome::Answered;
    else if (text == "ambiguous") out = VoiceOutcome::Ambiguous;
    else if (text == "clarification") out = VoiceOutcome::Clarification;
    else if (text == "not_found") out = VoiceOutcome::NotFound;
    else if (text == "unrelated") out = VoiceOutcome::Unrelated;
    else if (text == "failed") out = VoiceOutcome::Failed;
    else if (text == "unavailable") out = VoiceOutcome::Unavailable;
    else return false;
    return true;
}

// Known keys must carry the type TRAIT_STATE gives them; absent keys stay
// unknown. A mode or run state outside today's vocabulary is kept as Unknown so
// a newer Provider does not blank the panel.
bool ParseState(const cJSON* object, DeviceState& out) {
    if (!cJSON_IsObject(object)) return false;
    out = DeviceState{};
    const auto boolean = [&](const char* key, Reading<bool>& reading) {
        const cJSON* item = Field(object, key);
        if (item == nullptr) return true;
        if (!cJSON_IsBool(item)) return false;
        reading = {true, cJSON_IsTrue(item) != 0};
        return true;
    };
    const auto integer = [&](const char* key, int low, int high, bool nullable, Reading<int>& reading) {
        const cJSON* item = Field(object, key);
        if (item == nullptr || (nullable && cJSON_IsNull(item))) return true;
        int value = 0;
        if (!ReadInt(item, low, high, value)) return false;
        reading = {true, value};
        return true;
    };
    const auto number = [&](const char* key, bool nullable, Reading<double>& reading) {
        const cJSON* item = Field(object, key);
        if (item == nullptr || (nullable && cJSON_IsNull(item))) return true;
        double value = 0;
        if (!ReadNumber(item, value)) return false;
        reading = {true, value};
        return true;
    };
    const auto text = [&](const char* key, std::string& value) {
        const cJSON* item = Field(object, key);
        if (item == nullptr) return true;
        return ReadString(item, value);
    };
    std::string mode, run_state;
    const bool ok = boolean("on", out.on) && integer("level", 0, 100, false, out.level) && text("mode", mode) &&
        number("target_c", false, out.target_c) && number("current_c", true, out.current_c) &&
        integer("speed", 0, 100, false, out.speed) && integer("position", 0, 100, false, out.position) &&
        boolean("locked", out.locked) && text("run_state", run_state) &&
        integer("volume", 0, 100, false, out.volume) && boolean("muted", out.muted) &&
        number("temp_c", true, out.temp_c) && integer("humidity", 0, 100, true, out.humidity);
    if (!ok) return false;
    if (!mode.empty()) out.mode = ParseMode(mode);
    if (!run_state.empty()) out.run_state = ParseRunState(run_state);
    return true;
}

bool ParseOnline(const cJSON* object, bool& online) {
    const cJSON* item = Field(object, "online");
    if (item == nullptr) {
        online = true;
        return true;
    }
    if (!cJSON_IsBool(item)) return false;
    online = cJSON_IsTrue(item) != 0;
    return true;
}

bool ParseSnapshot(const cJSON* payload, Snapshot& out, const Failure& fail) {
    out = Snapshot{};
    if (!ReadRevision(Field(payload, "revision"), out.revision)) return fail("revision");
    if (!ReadRevision(Field(payload, "seq"), out.seq)) return fail("seq");
    if (const cJSON* home = Field(payload, "home_name"); home != nullptr && !ReadName(home, out.home_name))
        return fail("home_name");
    if (const cJSON* area = Field(payload, "panel_area_id"); area != nullptr && !cJSON_IsNull(area) &&
        !ReadIdentifier(area, out.panel_area_id))
        return fail("panel_area_id");
    if (const cJSON* offset = Field(payload, "utc_offset_minutes"); offset != nullptr) {
        int minutes = 0;
        if (!ReadInt(offset, -720, 840, minutes)) return fail("utc_offset_minutes");
        out.utc_offset_minutes = minutes;
    }

    const auto list = [&](const char* key, size_t max, const cJSON*& array) {
        array = Field(payload, key);
        if (array == nullptr) return true;  // SDK default: empty
        return cJSON_IsArray(array) && static_cast<size_t>(cJSON_GetArraySize(array)) <= max;
    };
    const cJSON* areas = nullptr;
    const cJSON* devices = nullptr;
    const cJSON* scenes = nullptr;
    if (!list("areas", kMaxAreas, areas)) return fail("areas");
    if (!list("devices", kMaxDevices, devices)) return fail("devices");
    if (!list("scenes", kMaxScenes, scenes)) return fail("scenes");

    std::set<std::string> area_ids, device_ids, scene_ids;
    out.areas.reserve(areas ? cJSON_GetArraySize(areas) : 0);
    for (const cJSON* item = areas ? areas->child : nullptr; item; item = item->next) {
        Area area;
        if (!cJSON_IsObject(item) || !ReadIdentifier(Field(item, "area_id"), area.id) ||
            !ReadName(Field(item, "name"), area.name))
            return fail("area");
        if (!area_ids.insert(area.id).second) return fail("DUPLICATE_AREA");
        out.areas.push_back(std::move(area));
    }
    if (!out.panel_area_id.empty() && !area_ids.count(out.panel_area_id)) return fail("PANEL_AREA_UNKNOWN");

    out.devices.reserve(devices ? cJSON_GetArraySize(devices) : 0);
    for (const cJSON* item = devices ? devices->child : nullptr; item; item = item->next) {
        Device device;
        std::string type;
        if (!cJSON_IsObject(item) || !ReadIdentifier(Field(item, "device_id"), device.id) ||
            !ReadName(Field(item, "name"), device.name) || !ReadString(Field(item, "type"), type) ||
            !ParseDeviceType(type, device.type))
            return fail("device");
        const cJSON* area = Field(item, "area_id");
        if (area != nullptr && !cJSON_IsNull(area) && !ReadIdentifier(area, device.area_id))
            return fail("device.area_id");
        if (!device.area_id.empty() && !area_ids.count(device.area_id)) return fail("DEVICE_AREA_UNKNOWN");
        if (!ParseOnline(item, device.online)) return fail("device.online");
        // State is absent only for an offline device nobody has reported yet.
        const cJSON* state = Field(item, "state");
        device.has_state = state != nullptr && !cJSON_IsNull(state);
        if (!device.has_state && device.online) return fail("ONLINE_DEVICE_NEEDS_STATE");
        if (device.has_state && !ParseState(state, device.state)) return fail("device.state");
        if (!device_ids.insert(device.id).second) return fail("DUPLICATE_DEVICE");
        out.devices.push_back(std::move(device));
    }

    out.scenes.reserve(scenes ? cJSON_GetArraySize(scenes) : 0);
    for (const cJSON* item = scenes ? scenes->child : nullptr; item; item = item->next) {
        Scene scene;
        if (!cJSON_IsObject(item) || !ReadIdentifier(Field(item, "scene_id"), scene.id) ||
            !ReadName(Field(item, "name"), scene.name))
            return fail("scene");
        if (!scene_ids.insert(scene.id).second) return fail("DUPLICATE_SCENE");
        out.scenes.push_back(std::move(scene));
    }
    return true;
}

bool ParseDelta(const cJSON* payload, Delta& out, const Failure& fail) {
    out = Delta{};
    if (!ReadRevision(Field(payload, "revision"), out.revision)) return fail("revision");
    if (!ReadRevision(Field(payload, "seq"), out.seq) || out.seq < 1) return fail("seq");
    const cJSON* source = Field(payload, "source");
    std::string kind;
    if (!cJSON_IsObject(source) || !ReadString(Field(source, "kind"), kind) || !ParseOrigin(kind, out.source_kind))
        return fail("source");
    if (const cJSON* label = Field(source, "label"); label != nullptr && !cJSON_IsNull(label)) {
        if (!ReadString(label, out.source_label) || CodePoints(out.source_label) > 32) return fail("source.label");
    }
    const cJSON* changes = Field(payload, "changes");
    const int count = cJSON_IsArray(changes) ? cJSON_GetArraySize(changes) : 0;
    if (count < 1 || static_cast<size_t>(count) > kMaxDeltaChanges) return fail("changes");
    std::set<std::string> ids;
    out.changes.reserve(count);
    for (const cJSON* item = changes->child; item; item = item->next) {
        Change change;
        if (!cJSON_IsObject(item) || !ReadIdentifier(Field(item, "device_id"), change.device_id) ||
            !ParseOnline(item, change.online) || !ParseState(Field(item, "state"), change.state))
            return fail("change");
        if (!ids.insert(change.device_id).second) return fail("DUPLICATE_CHANGE");
        out.changes.push_back(std::move(change));
    }
    return true;
}

bool ParseParams(const cJSON* object, std::vector<CommandParam>& out) {
    out.clear();
    if (object == nullptr) return true;
    if (!cJSON_IsObject(object)) return false;
    for (const cJSON* item = object->child; item; item = item->next) {
        CommandParam param;
        param.name = item->string ? item->string : "";
        if (param.name.empty()) return false;
        if (cJSON_IsBool(item)) {
            param.kind = CommandParam::Kind::Bool;
            param.boolean = cJSON_IsTrue(item) != 0;
        } else if (cJSON_IsNumber(item) && std::isfinite(item->valuedouble)) {
            param.kind = CommandParam::Kind::Number;
            param.number = item->valuedouble;
        } else if (cJSON_IsString(item) && item->valuestring) {
            param.kind = CommandParam::Kind::String;
            param.text = item->valuestring;
        } else {
            return false;
        }
        out.push_back(std::move(param));
    }
    return true;
}

bool ParseVoiceResult(const cJSON* payload, VoiceResult& out, const Failure& fail) {
    out = VoiceResult{};
    std::string outcome;
    if (!ReadIdentifier(Field(payload, "turn_id"), out.turn_id)) return fail("turn_id");
    if (!ReadString(Field(payload, "utterance"), out.utterance) || CodePoints(out.utterance) > 200)
        return fail("utterance");
    if (!ReadString(Field(payload, "outcome"), outcome) || !ParseOutcome(outcome, out.outcome))
        return fail("outcome");
    if (!ReadString(Field(payload, "message"), out.message) || out.message.empty() ||
        CodePoints(out.message) > 120)
        return fail("message");
    if (const cJSON* candidates = Field(payload, "candidates"); candidates != nullptr) {
        if (!cJSON_IsArray(candidates) || static_cast<size_t>(cJSON_GetArraySize(candidates)) > kMaxCandidates)
            return fail("candidates");
        std::set<std::string> ids;
        for (const cJSON* item = candidates->child; item; item = item->next) {
            Candidate candidate;
            if (!cJSON_IsObject(item) || !ReadIdentifier(Field(item, "device_id"), candidate.device_id) ||
                !ReadName(Field(item, "name"), candidate.name))
                return fail("candidate");
            if (!ids.insert(candidate.device_id).second) return fail("DUPLICATE_CANDIDATE");
            out.candidates.push_back(std::move(candidate));
        }
    }
    if (const cJSON* command = Field(payload, "command"); command != nullptr && !cJSON_IsNull(command)) {
        if (!cJSON_IsObject(command) || !ReadIdentifier(Field(command, "trait"), out.command.trait) ||
            !ReadIdentifier(Field(command, "command"), out.command.command) ||
            !ParseParams(Field(command, "params"), out.command.params))
            return fail("command");
        out.has_command = true;
    }
    const bool ambiguous = out.outcome == VoiceOutcome::Ambiguous;
    if (ambiguous != (out.candidates.size() >= 2)) return fail("CANDIDATES_DO_NOT_MATCH_OUTCOME");
    if (ambiguous != out.has_command) return fail("COMMAND_TEMPLATE_DO_NOT_MATCH_OUTCOME");
    return true;
}

bool AddParams(cJSON* object, const std::vector<CommandParam>& params) {
    for (const auto& param : params) {
        if (param.name.empty() || Field(object, param.name.c_str()) != nullptr) return false;
        cJSON* added = nullptr;
        switch (param.kind) {
        case CommandParam::Kind::Bool:
            added = cJSON_AddBoolToObject(object, param.name.c_str(), param.boolean);
            break;
        case CommandParam::Kind::Number:
            if (!std::isfinite(param.number)) return false;
            added = cJSON_AddNumberToObject(object, param.name.c_str(), param.number);
            break;
        case CommandParam::Kind::String:
            added = cJSON_AddStringToObject(object, param.name.c_str(), param.text.c_str());
            break;
        }
        if (added == nullptr) return false;
    }
    return true;
}

std::string Print(cJSON* root) {
    char* raw = root ? cJSON_PrintUnformatted(root) : nullptr;
    std::string out = raw ? raw : "";
    if (raw) cJSON_free(raw);
    return out;
}

}  // namespace

bool IsSmartHomeOp(const std::string& op) {
    return op == kOpSnapshot || op == kOpDelta || op == kOpResult;
}

bool IsIdentifier(const std::string& value) {
    if (value.empty() || value.size() > 128) return false;
    for (unsigned char c : value) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '_' || c == ':' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool ParseMessage(const std::string& op, const std::string& payload_json, Message& out, std::string* error) {
    const Failure fail{error};
    if (error != nullptr) error->clear();
    MessageKind kind;
    if (op == kOpSnapshot) kind = MessageKind::Snapshot;
    else if (op == kOpDelta) kind = MessageKind::Delta;
    else if (op == kOpResult) kind = MessageKind::Result;
    else return fail("UNKNOWN_OP");
    const size_t cap = kind == MessageKind::Result ? kMaxResultPayloadBytes : kMaxStatePayloadBytes;
    if (payload_json.size() > cap) return fail("PAYLOAD_TOO_LARGE");

    JsonPtr root(cJSON_ParseWithLength(payload_json.data(), payload_json.size()), cJSON_Delete);
    if (!root || !cJSON_IsObject(root.get())) return fail("MALFORMED_JSON");
    if (!SchemaVersionOk(root.get())) return fail("schema_version");
    out.kind = kind;
    switch (kind) {
    case MessageKind::Snapshot:
        return ParseSnapshot(root.get(), out.snapshot, fail);
    case MessageKind::Delta:
        return ParseDelta(root.get(), out.delta, fail);
    case MessageKind::Result:
        return ParseVoiceResult(root.get(), out.result, fail);
    }
    return fail("UNKNOWN_OP");
}

namespace {

// {"schema_v":1,"type":<type>,"payload":<payload>}; takes ownership of payload.
std::string Request(const char* type, cJSON* payload) {
    JsonPtr root(cJSON_CreateObject(), cJSON_Delete);
    if (!root || payload == nullptr) {
        cJSON_Delete(payload);
        return "";
    }
    cJSON_AddNumberToObject(root.get(), "schema_v", kSchemaVersion);
    cJSON_AddStringToObject(root.get(), "type", type);
    cJSON_AddItemToObject(root.get(), "payload", payload);
    return Print(root.get());
}

cJSON* ExecutePayload(const std::string& request_id) {
    if (!IsIdentifier(request_id)) return nullptr;
    cJSON* payload = cJSON_CreateObject();
    if (payload == nullptr) return nullptr;
    cJSON_AddNumberToObject(payload, "schema_version", kSchemaVersion);
    cJSON_AddStringToObject(payload, "request_id", request_id.c_str());
    return payload;
}

}  // namespace

std::string BuildExecuteRequest(const std::string& request_id, const std::vector<Command>& commands) {
    if (commands.empty() || commands.size() > kMaxPanelCommands) return "";
    JsonPtr payload(ExecutePayload(request_id), cJSON_Delete);
    if (!payload) return "";
    cJSON* list = cJSON_AddArrayToObject(payload.get(), "commands");
    if (list == nullptr) return "";
    for (const auto& command : commands) {
        if (!IsIdentifier(command.device_id) || !IsIdentifier(command.trait) || !IsIdentifier(command.command))
            return "";
        cJSON* item = cJSON_CreateObject();
        if (item == nullptr) return "";
        cJSON_AddItemToArray(list, item);
        cJSON_AddStringToObject(item, "device_id", command.device_id.c_str());
        cJSON_AddStringToObject(item, "trait", command.trait.c_str());
        cJSON_AddStringToObject(item, "command", command.command.c_str());
        cJSON* params = cJSON_AddObjectToObject(item, "params");
        if (params == nullptr || !AddParams(params, command.params)) return "";
    }
    cJSON_AddNullToObject(payload.get(), "scene_id");
    return Request(kRequestExecute, payload.release());
}

std::string BuildSceneRequest(const std::string& request_id, const std::string& scene_id) {
    if (!IsIdentifier(scene_id)) return "";
    JsonPtr payload(ExecutePayload(request_id), cJSON_Delete);
    if (!payload || cJSON_AddArrayToObject(payload.get(), "commands") == nullptr) return "";
    cJSON_AddStringToObject(payload.get(), "scene_id", scene_id.c_str());
    return Request(kRequestExecute, payload.release());
}

std::string BuildSyncRequest(bool has_snapshot, uint64_t known_revision, uint64_t known_seq) {
    cJSON* payload = cJSON_CreateObject();
    if (payload == nullptr) return "";
    cJSON_AddNumberToObject(payload, "schema_version", kSchemaVersion);
    if (has_snapshot) {
        cJSON_AddNumberToObject(payload, "known_revision", static_cast<double>(known_revision));
        cJSON_AddNumberToObject(payload, "known_seq", static_cast<double>(known_seq));
    } else {
        cJSON_AddNullToObject(payload, "known_revision");
        cJSON_AddNullToObject(payload, "known_seq");
    }
    return Request(kRequestSync, payload);
}

std::string RequestIdSource::Next() {
    char id[40];
    std::snprintf(id, sizeof(id), "touch-%08x-%u", static_cast<unsigned>(nonce_), static_cast<unsigned>(++counter_));
    return id;
}

}  // namespace eidolon::smarthome
