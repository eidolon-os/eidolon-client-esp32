#ifndef EIDOLON_SMARTHOME_MODEL_H_
#define EIDOLON_SMARTHOME_MODEL_H_

#include <cstdint>
#include <string>
#include <vector>

// Smart home panel wire, as the panel holds it (eidolon_sdk biz.smarthome v1,
// "Panel wire"). The panel owns none of these facts: the registry belongs to the
// Owner, device state to its Provider. Everything here is a cache of what the
// host last sent, and is replaced, never merged, by the next snapshot.
//
// Pure data: no ESP-IDF, no LVGL. Host tests read it against the SDK goldens.
namespace eidolon::smarthome {

// Bounds the SDK validates on the host; the panel re-checks them so a producer
// bug cannot grow the cache without limit.
constexpr size_t kMaxAreas = 32;
constexpr size_t kMaxDevices = 128;
constexpr size_t kMaxScenes = 16;
constexpr size_t kMaxDeltaChanges = 32;
constexpr size_t kMaxCandidates = 8;
constexpr size_t kMaxPanelCommands = 8;

enum class DeviceType : uint8_t {
    Light,
    Switch,
    Climate,
    WaterHeater,
    Cover,
    Fan,
    Media,
    Appliance,
    Lock,
    Camera,
    Sensor,
};

enum class ThermostatMode : uint8_t { Unknown, Cool, Heat, Auto, Fan, Dry };
enum class RunState : uint8_t { Unknown, Idle, Running, Paused, Docked };
enum class OriginKind : uint8_t { Voice, Text, Touch, Scene, Mobile, Automation };
enum class VoiceOutcome : uint8_t {
    Executed, Partial, Answered, Ambiguous, NotFound, Unrelated, Failed, Unavailable, Clarification,
};

// A state value that may be absent from the wire or sent as JSON null. The
// panel never needs to tell those two apart: both mean "nothing to show".
template <typename T>
struct Reading {
    bool known = false;
    T value{};
};

// Union of the state keys of every trait (SDK TRAIT_STATE). Which ones a
// device carries is decided by its type; the rest stay unknown.
struct DeviceState {
    Reading<bool> on;              // on_off
    Reading<int> level;            // level, 0..100
    ThermostatMode mode = ThermostatMode::Unknown;  // thermostat
    Reading<double> target_c;      // thermostat
    Reading<double> current_c;     // thermostat (nullable)
    Reading<int> speed;            // fan_speed, 0..100
    Reading<int> position;         // position, 0..100
    Reading<bool> locked;          // lock
    RunState run_state = RunState::Unknown;  // operational
    Reading<int> volume;           // volume, 0..100
    Reading<bool> muted;           // volume
    Reading<double> temp_c;        // measure (nullable)
    Reading<int> humidity;         // measure (nullable)
};

struct Area {
    std::string id;
    std::string name;
};

struct Device {
    std::string id;
    std::string name;
    std::string area_id;  // empty: not placed in any area
    DeviceType type = DeviceType::Switch;
    bool online = true;
    // False only for an offline device nobody has reported yet (the wire
    // carries no state for it); such a device shows as offline and nothing else.
    bool has_state = true;
    DeviceState state;
};

struct Scene {
    std::string id;
    std::string name;
};

// smarthome.snapshot: everything a panel shows.
struct Snapshot {
    uint64_t revision = 0;
    uint64_t seq = 0;
    std::string home_name = "我的家";
    std::string panel_area_id;  // empty: the panel is not placed
    int32_t utc_offset_minutes = 0;  // the home's local time, -720..840
    std::vector<Area> areas;
    std::vector<Device> devices;
    std::vector<Scene> scenes;
};

// One device's full state after a change, so applying it is idempotent.
struct Change {
    std::string device_id;
    bool online = true;
    DeviceState state;
};

// smarthome.delta: state changes only; a registry change is always a snapshot.
struct Delta {
    uint64_t revision = 0;
    uint64_t seq = 0;
    OriginKind source_kind = OriginKind::Automation;
    std::string source_label;  // may be empty
    std::vector<Change> changes;
};

struct CommandParam {
    enum class Kind : uint8_t { Number, Bool, String };
    std::string name;
    Kind kind = Kind::Number;
    double number = 0;
    bool boolean = false;
    std::string text;
};

// A command against one device (SDK Command). For a voice result's template
// the device_id is empty until a candidate is chosen.
struct Command {
    std::string device_id;
    std::string trait;
    std::string command;
    std::vector<CommandParam> params;
};

struct Candidate {
    std::string device_id;
    std::string name;
};

// smarthome.result: what one spoken command came to, for the result card.
struct VoiceResult {
    std::string turn_id;
    std::string utterance;
    VoiceOutcome outcome = VoiceOutcome::Executed;
    std::string message;
    std::vector<Candidate> candidates;
    bool has_command = false;
    Command command;  // template; device_id empty
};

}  // namespace eidolon::smarthome

#endif  // EIDOLON_SMARTHOME_MODEL_H_
