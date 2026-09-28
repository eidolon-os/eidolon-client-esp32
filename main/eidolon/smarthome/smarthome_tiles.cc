#include "smarthome_tiles.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace eidolon::smarthome {

namespace {

std::string Format(const char* pattern, ...) __attribute__((format(printf, 1, 2)));
std::string Format(const char* pattern, ...) {
    char buffer[96];
    va_list args;
    va_start(args, pattern);
    std::vsnprintf(buffer, sizeof(buffer), pattern, args);
    va_end(args);
    return buffer;
}

// 26 -> "26", 26.5 -> "26.5".
std::string Degrees(double value) {
    const double rounded = std::round(value * 10) / 10;
    if (std::fabs(rounded - std::round(rounded)) < 1e-9) return Format("%d", static_cast<int>(std::lround(rounded)));
    return Format("%.1f", rounded);
}

const char* ModeLabel(ThermostatMode mode) {
    switch (mode) {
    case ThermostatMode::Cool: return "制冷";
    case ThermostatMode::Heat: return "制热";
    case ThermostatMode::Auto: return "自动";
    case ThermostatMode::Fan: return "送风";
    case ThermostatMode::Dry: return "除湿";
    case ThermostatMode::Unknown: break;
    }
    return "开";
}

const char* RunStateLabel(RunState state) {
    switch (state) {
    case RunState::Idle: return "空闲";
    case RunState::Running: return "运行中";
    case RunState::Paused: return "已暂停";
    case RunState::Docked: return "已回充";
    case RunState::Unknown: break;
    }
    return "--";
}

const char* OnOff(const Reading<bool>& on) {
    return !on.known ? "--" : on.value ? "开" : "关";
}

std::string ClimateText(const DeviceState& s, bool compact) {
    std::string room = s.current_c.known && !compact ? " · 室温 " + Degrees(s.current_c.value) + "°" : "";
    if (!s.on.known) return "--";
    if (!s.on.value) return "关" + room;
    std::string text = ModeLabel(s.mode);
    if (s.target_c.known) text += " " + Degrees(s.target_c.value) + "°C";
    return text + room;
}

std::string CoverText(const Reading<int>& position) {
    if (!position.known) return "--";
    if (position.value <= 0) return "已关闭";
    if (position.value >= 100) return "全开";
    return Format("开 %d%%", position.value);
}

std::string SensorText(const DeviceState& s) {
    if (s.temp_c.known && s.humidity.known) return Format("%.1f°C · %d%%", s.temp_c.value, s.humidity.value);
    if (s.temp_c.known) return Format("%.1f°C", s.temp_c.value);
    if (s.humidity.known) return Format("湿度 %d%%", s.humidity.value);
    return "暂无读数";
}

Command Make(const Device& device, const char* trait, const char* command) {
    Command out;
    out.device_id = device.id;
    out.trait = trait;
    out.command = command;
    return out;
}

Command MakeNumber(const Device& device, const char* trait, const char* command, const char* name, double value) {
    Command out = Make(device, trait, command);
    CommandParam param;
    param.name = name;
    param.kind = CommandParam::Kind::Number;
    param.number = value;
    out.params.push_back(std::move(param));
    return out;
}

// Absolute set from the cached value; false when the cache cannot say or the
// value would not move.
bool StepAbsolute(const Device& device, const Reading<int>& current, int delta, int low, int high,
                  const char* trait, Command& out) {
    if (!current.known) return false;
    const int next = std::clamp(current.value + delta, low, high);
    if (next == current.value) return false;
    out = MakeNumber(device, trait, "set", "value", next);
    return true;
}

}  // namespace

const char* DeviceGlyph(const Device& device) {
    // A few common appliances get their own character, as a person would
    // label them; everything else takes its type's.
    static constexpr struct { const char* word; const char* glyph; } kNamed[] = {
        {"加湿", "湿"}, {"净化", "净"}, {"音箱", "音"}, {"电视", "视"}, {"扫地", "扫"},
        {"洗衣", "洗"}, {"电饭", "饭"}, {"晾衣", "晾"},
    };
    for (const auto& entry : kNamed) {
        if (device.name.find(entry.word) != std::string::npos) return entry.glyph;
    }
    switch (device.type) {
    case DeviceType::Light: return "灯";
    case DeviceType::Switch: return "开";
    case DeviceType::Climate: return device.state.mode == ThermostatMode::Heat ? "暖" : "冷";
    case DeviceType::WaterHeater: return "热";
    case DeviceType::Cover: return "帘";
    case DeviceType::Fan: return "风";
    case DeviceType::Media: return "视";
    case DeviceType::Appliance: return "机";
    case DeviceType::Lock: return "锁";
    case DeviceType::Camera: return "摄";
    case DeviceType::Sensor: return "温";
    }
    return "?";
}

std::string DeviceStateText(const Device& device, bool compact) {
    if (!device.online || !device.has_state) return "离线";
    const DeviceState& s = device.state;
    switch (device.type) {
    case DeviceType::Light:
        if (s.on.known && s.on.value && s.level.known) return Format("开 · %d%%", s.level.value);
        return OnOff(s.on);
    case DeviceType::Switch:
        return OnOff(s.on);
    case DeviceType::Climate:
        return ClimateText(s, compact);
    case DeviceType::WaterHeater:
        if (!s.on.known || !s.on.value) return OnOff(s.on);
        return s.target_c.known ? "加热 · " + Degrees(s.target_c.value) + "°C" : "加热";
    case DeviceType::Cover:
        return CoverText(s.position);
    case DeviceType::Fan:
        if (s.on.known && s.on.value) return s.speed.known ? Format("运行 · %d%%", s.speed.value) : "运行";
        return OnOff(s.on);
    case DeviceType::Media:
        if (s.on.known && s.on.value) {
            if (s.muted.known && s.muted.value) return "开 · 静音";
            return s.volume.known ? Format("开 · 音量 %d", s.volume.value) : "开";
        }
        return OnOff(s.on);
    case DeviceType::Appliance:
        return RunStateLabel(s.run_state);
    case DeviceType::Lock:
        return !s.locked.known ? "--" : s.locked.value ? "已上锁" : "未上锁";
    case DeviceType::Camera:
        return !s.on.known ? "--" : s.on.value ? "看护中" : "已关闭";
    case DeviceType::Sensor:
        return SensorText(s);
    }
    return "--";
}

bool IsActive(const Device& device) {
    if (!device.online || !device.has_state) return false;
    const DeviceState& s = device.state;
    switch (device.type) {
    case DeviceType::Cover: return s.position.known && s.position.value > 0;
    case DeviceType::Appliance: return s.run_state == RunState::Running;
    case DeviceType::Lock: return s.locked.known && !s.locked.value;
    case DeviceType::Sensor: return false;
    default: return s.on.known && s.on.value;
    }
}

bool OffersSteps(const Device& device) {
    if (!device.online || !device.has_state) return false;
    switch (device.type) {
    case DeviceType::Light:
    case DeviceType::Climate:
    case DeviceType::WaterHeater:
    case DeviceType::Cover:
    case DeviceType::Fan:
    case DeviceType::Media:
        return true;
    default:
        return false;
    }
}

bool TileCommand(const Device& device, TileAction action, Command& out) {
    if (!device.online || !device.has_state) return false;
    const DeviceState& s = device.state;
    if (action == TileAction::Tap) {
        switch (device.type) {
        case DeviceType::Cover:
            out = Make(device, "position", s.position.known && s.position.value > 0 ? "close" : "open");
            return true;
        case DeviceType::Appliance:
            out = Make(device, "operational", s.run_state == RunState::Running ? "pause" : "start");
            return true;
        case DeviceType::Lock:
            out = Make(device, "lock", s.locked.known && s.locked.value ? "unlock" : "lock");
            return true;
        case DeviceType::Sensor:
            return false;
        default:
            out = Make(device, "on_off", "toggle");
            return true;
        }
    }
    if (!OffersSteps(device)) return false;
    const int direction = action == TileAction::Plus ? 1 : -1;
    switch (device.type) {
    case DeviceType::Light:
        out = MakeNumber(device, "level", "step", "delta", 20 * direction);
        return true;
    case DeviceType::Climate:
    case DeviceType::WaterHeater:
        out = MakeNumber(device, "thermostat", "step", "delta", direction);
        return true;
    case DeviceType::Cover:
        return StepAbsolute(device, s.position, 20 * direction, 0, 100, "position", out);
    case DeviceType::Fan:
        return StepAbsolute(device, s.speed, 10 * direction, 10, 100, "fan_speed", out);
    case DeviceType::Media:
        out = MakeNumber(device, "volume", "step", "delta", 5 * direction);
        return true;
    default:
        return false;
    }
}

std::string DescribeChange(const Device& before, const Change& after) {
    const DeviceState& a = before.state;
    const DeviceState& b = after.state;
    if (before.online != after.online) return after.online ? "已上线" : "离线";
    if (!before.has_state) {
        Device now = before;
        now.online = after.online;
        now.has_state = true;
        now.state = after.state;
        return DeviceStateText(now);
    }
    const auto changed = [](const auto& x, const auto& y) { return x.known != y.known || x.value != y.value; };
    if (b.on.known && changed(a.on, b.on)) return b.on.value ? "已打开" : "已关闭";
    if (b.locked.known && changed(a.locked, b.locked)) return b.locked.value ? "已上锁" : "已开锁";
    if (b.position.known && changed(a.position, b.position)) {
        if (b.position.value <= 0) return "已关闭";
        if (b.position.value >= 100) return "已全开";
        return Format("开到 %d%%", b.position.value);
    }
    if (b.run_state != RunState::Unknown && a.run_state != b.run_state) {
        switch (b.run_state) {
        case RunState::Running: return "开始运行";
        case RunState::Paused: return "已暂停";
        case RunState::Idle: return "已停止";
        case RunState::Docked: return "已回充";
        case RunState::Unknown: break;
        }
    }
    if (b.mode != ThermostatMode::Unknown && a.mode != b.mode) return std::string("切换为") + ModeLabel(b.mode);
    if (b.target_c.known && changed(a.target_c, b.target_c)) return "设为 " + Degrees(b.target_c.value) + "°C";
    if (b.level.known && changed(a.level, b.level)) return Format("调到 %d%%", b.level.value);
    if (b.speed.known && changed(a.speed, b.speed)) return Format("风速 %d%%", b.speed.value);
    if (b.muted.known && changed(a.muted, b.muted)) return b.muted.value ? "已静音" : "取消静音";
    if (b.volume.known && changed(a.volume, b.volume)) return Format("音量 %d", b.volume.value);
    Device now = before;
    now.online = after.online;
    now.has_state = true;
    now.state = after.state;
    return DeviceStateText(now);
}

const char* OriginLabel(OriginKind kind) {
    switch (kind) {
    case OriginKind::Voice: return "语音";
    case OriginKind::Text: return "文字";
    case OriginKind::Touch: return "触控";
    case OriginKind::Scene: return "场景";
    case OriginKind::Mobile: return "手机";
    case OriginKind::Automation: return "自动化";
    }
    return "";
}

ResultTone OutcomeTone(VoiceOutcome outcome) {
    switch (outcome) {
    case VoiceOutcome::Executed:
    case VoiceOutcome::Answered:
    case VoiceOutcome::Ambiguous:
    case VoiceOutcome::Clarification:
        return ResultTone::Normal;
    case VoiceOutcome::Partial:
        return ResultTone::Attention;
    case VoiceOutcome::NotFound:
    case VoiceOutcome::Unrelated:
    case VoiceOutcome::Failed:
    case VoiceOutcome::Unavailable:
        break;
    }
    return ResultTone::Error;
}

std::string FormatClock(int64_t utc_ms, int32_t utc_offset_minutes) {
    if (utc_ms <= 0) return "--:--";
    const int64_t local_s = utc_ms / 1000 + static_cast<int64_t>(utc_offset_minutes) * 60;
    const int64_t day_s = ((local_s % 86400) + 86400) % 86400;
    return Format("%02d:%02d", static_cast<int>(day_s / 3600), static_cast<int>(day_s % 3600 / 60));
}

std::string FormatActivity(const Activity& activity, int32_t utc_offset_minutes) {
    if (!activity.valid) return "";
    std::string line;
    if (activity.utc_ms > 0) line = FormatClock(activity.utc_ms, utc_offset_minutes) + " ";
    line += activity.device_name + " " + activity.change;
    if (activity.more > 0) line += Format(" 等 %u 个设备", static_cast<unsigned>(activity.more + 1));
    if (!activity.source.empty()) line += " · 来自 " + activity.source;
    return line;
}

bool InArea(const Device& device, const std::string& area_id) {
    if (area_id.empty()) return true;
    if (area_id == kUnplacedArea) return device.area_id.empty();
    return device.area_id == area_id;
}

std::vector<NavEntry> BuildNav(const Snapshot& home) {
    std::vector<NavEntry> nav;
    nav.reserve(home.areas.size() + 2);
    nav.push_back({kAllAreas, "全部", home.devices.size()});
    for (const auto& area : home.areas) {
        const size_t count = std::count_if(home.devices.begin(), home.devices.end(),
                                           [&](const Device& d) { return d.area_id == area.id; });
        if (count > 0) nav.push_back({area.id, area.name, count});
    }
    const size_t unplaced = std::count_if(home.devices.begin(), home.devices.end(),
                                          [](const Device& d) { return d.area_id.empty(); });
    if (unplaced > 0) nav.push_back({kUnplacedArea, "其他", unplaced});
    return nav;
}

std::string DefaultArea(const Snapshot& home) {
    if (home.panel_area_id.empty()) return kAllAreas;
    for (const auto& device : home.devices) {
        if (device.area_id == home.panel_area_id) return home.panel_area_id;
    }
    return kAllAreas;
}

}  // namespace eidolon::smarthome
