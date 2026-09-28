// Smart home panel core against the SDK's own wire vectors.
//
// Downward vectors are full eidolon.control envelopes and go through the
// firmware's real control parser first, exactly as a data-channel packet would;
// upward vectors are bare payloads that the builders must reproduce.
#include "eidolon/control_protocol.h"
#include "eidolon/smarthome/smarthome_store.h"
#include "eidolon/smarthome/smarthome_tiles.h"
#include "eidolon/smarthome/smarthome_wire.h"

#include <cJSON.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace eidolon::smarthome;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)
#define CHECK_EQ(actual, expected)                                               \
    do {                                                                         \
        const auto a_ = (actual);                                                \
        const auto e_ = (expected);                                              \
        if (!(a_ == e_)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s\n  actual:   %s\n  expected: %s\n", \
                         __FILE__, __LINE__, #actual, Show(a_).c_str(), Show(e_).c_str()); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

std::string Show(const std::string& v) { return "\"" + v + "\""; }
template <typename T>
std::string Show(const T& v) { return std::to_string(v); }

constexpr int64_t kUtc = 1700000000000LL;  // the goldens' issue time
constexpr int32_t kChina = 8 * 60;

std::string ReadGolden(const std::string& name) {
    std::ifstream file("../eidolon_sdk/contracts/smarthome/v1/golden/" + name);
    if (!file.is_open()) {
        std::fprintf(stderr, "missing golden %s (run from the repo root with ../eidolon_sdk checked out)\n",
                     name.c_str());
        std::exit(2);
    }
    std::ostringstream bytes;
    bytes << file.rdbuf();
    return bytes.str();
}

// A control envelope, through the firmware's own control parser.
Message Receive(const std::string& golden) {
    const auto command = eidolon::ParseControlCommand(ReadGolden(golden), kUtc + 500);
    CHECK(command.valid && command.is_v1 && !command.expired);
    CHECK(command.capability_version == kCapabilityVersion);
    CHECK(IsSmartHomeOp(command.op));
    Message message;
    std::string error;
    const bool parsed = ParseMessage(command.op, command.payload, message, &error);
    if (!parsed) std::fprintf(stderr, "%s rejected: %s\n", golden.c_str(), error.c_str());
    CHECK(parsed);
    return message;
}

bool SameJson(const std::string& a, const std::string& b) {
    cJSON* x = cJSON_Parse(a.c_str());
    cJSON* y = cJSON_Parse(b.c_str());
    const bool same = x && y && cJSON_Compare(x, y, true);
    if (!same) std::fprintf(stderr, "JSON differs:\n  %s\n  %s\n", a.c_str(), b.c_str());
    cJSON_Delete(x);
    cJSON_Delete(y);
    return same;
}

// The payload of an envelope golden, re-printed, for mutation tests.
std::string PayloadOf(const std::string& golden) {
    return eidolon::ParseControlCommand(ReadGolden(golden), kUtc + 500).payload;
}

std::string Mutate(const std::string& payload, void (*edit)(cJSON*)) {
    cJSON* root = cJSON_Parse(payload.c_str());
    edit(root);
    char* raw = cJSON_PrintUnformatted(root);
    std::string out(raw);
    cJSON_free(raw);
    cJSON_Delete(root);
    return out;
}

bool Rejects(const char* op, const std::string& payload) {
    Message message;
    return !ParseMessage(op, payload, message);
}

const Device& Find(const SmartHomeStore& store, const char* id) {
    const Device* device = store.FindDevice(id);
    if (device == nullptr) {
        std::fprintf(stderr, "device %s missing\n", id);
        std::exit(2);
    }
    return *device;
}

std::string Describe(const Command& c) {
    std::string out = c.device_id + " " + c.trait + "." + c.command;
    for (const auto& p : c.params) {
        char value[32];
        std::snprintf(value, sizeof(value), "%g", p.number);
        out += " " + p.name + "=" + value;
    }
    return out;
}

std::string CommandFor(const Device& device, TileAction action) {
    Command command;
    return TileCommand(device, action, command) ? Describe(command) : "none";
}

Device Synthetic(DeviceType type, const DeviceState& state, const char* name = "测试设备") {
    Device device;
    device.id = "test.device";
    device.name = name;
    device.type = type;
    device.state = state;
    return device;
}

SmartHomeStore LoadedStore() {
    SmartHomeStore store;
    auto message = Receive("panel-snapshot.json");
    CHECK(store.ApplySnapshot(std::move(message.snapshot)) == ApplyOutcome::Applied);
    return store;
}

void TestSnapshotGolden() {
    const Message message = Receive("panel-snapshot.json");
    CHECK(message.kind == MessageKind::Snapshot);
    const Snapshot& s = message.snapshot;
    CHECK_EQ(s.revision, 1ull);
    CHECK_EQ(s.seq, 0ull);
    CHECK_EQ(s.home_name, std::string("我的家"));
    CHECK_EQ(s.panel_area_id, std::string("living"));
    CHECK_EQ(s.utc_offset_minutes, 480);
    CHECK_EQ(s.areas.size(), size_t{7});
    CHECK_EQ(s.devices.size(), size_t{18});
    CHECK_EQ(s.scenes.size(), size_t{4});
    CHECK_EQ(s.areas[0].name, std::string("客厅"));
    CHECK_EQ(s.scenes[3].name, std::string("睡眠"));
    const Device& heater = s.devices[14];
    CHECK_EQ(heater.id, std::string("bath.water_heater"));
    CHECK(heater.type == DeviceType::WaterHeater && heater.online);
    CHECK(heater.state.on.known && heater.state.on.value);
    CHECK(heater.state.mode == ThermostatMode::Heat);
    CHECK(heater.state.target_c.known && heater.state.target_c.value == 42);
    CHECK(!heater.state.current_c.known);  // JSON null
    // Offline and never reported: no state on the wire, nothing but "离线".
    const Device& camera = s.devices[16];
    CHECK_EQ(camera.id, std::string("entry.camera"));
    CHECK(!camera.online && !camera.has_state);
    const Device& thermo = s.devices[17];
    CHECK(thermo.type == DeviceType::Sensor && thermo.state.temp_c.value == 24.5 &&
          thermo.state.humidity.value == 48);
}

void TestDeltaGoldenAndSequence() {
    SmartHomeStore store = LoadedStore();
    CHECK(store.has_snapshot() && !store.stale() && store.link_up());
    CHECK(!Find(store, "living.ac").state.on.value);

    const Message delta = Receive("panel-delta.json");
    CHECK(delta.kind == MessageKind::Delta);
    CHECK(delta.delta.source_kind == OriginKind::Voice);
    const uint32_t before = store.generation();
    CHECK(store.ApplyDelta(delta.delta, kUtc) == ApplyOutcome::Applied);
    CHECK(store.generation() != before);
    CHECK_EQ(store.seq(), 1ull);
    const Device& ac = Find(store, "living.ac");
    CHECK(ac.state.on.value && ac.state.target_c.value == 26 && ac.state.current_c.value == 28);
    CHECK_EQ(DeviceStateText(ac), std::string("制冷 26°C · 室温 28°"));
    CHECK_EQ(DeviceStateText(ac, true), std::string("制冷 26°C"));
    CHECK_EQ(store.activity().device_name, std::string("客厅空调"));
    CHECK_EQ(store.activity().change, std::string("已打开"));
    CHECK_EQ(store.activity().source, std::string("面板语音"));
    CHECK_EQ(FormatActivity(store.activity(), store.home().utc_offset_minutes),
             std::string("06:13 客厅空调 已打开 · 来自 面板语音"));

    // A repeat (seq at or below the last) is ignored: no sync, nothing moves.
    const uint32_t applied = store.generation();
    Delta repeat = delta.delta;
    repeat.changes[0].state.on.value = false;
    CHECK(store.ApplyDelta(repeat, kUtc) == ApplyOutcome::Ignored);
    CHECK_EQ(store.seq(), 1ull);
    CHECK(Find(store, "living.ac").state.on.value);
    CHECK(store.generation() == applied);
    CHECK_EQ(store.activity().change, std::string("已打开"));

    Delta gap = delta.delta;
    gap.seq = 3;
    gap.changes[0].state.on.value = false;
    CHECK(store.ApplyDelta(gap, kUtc) == ApplyOutcome::NeedSync);
    CHECK(Find(store, "living.ac").state.on.value);

    Delta other_revision = delta.delta;
    other_revision.revision = 2;
    other_revision.seq = 2;
    other_revision.changes[0].state.on.value = false;
    CHECK(store.ApplyDelta(other_revision, kUtc) == ApplyOutcome::NeedSync);
    CHECK(Find(store, "living.ac").state.on.value);

    // One unknown device fails the whole delta: nothing is half-applied.
    Delta partial = delta.delta;
    partial.seq = 2;
    partial.changes[0].state.on.value = false;
    Change ghost = partial.changes[0];
    ghost.device_id = "garage.door";
    partial.changes.push_back(ghost);
    CHECK(store.ApplyDelta(partial, kUtc) == ApplyOutcome::NeedSync);
    CHECK(Find(store, "living.ac").state.on.value);
    CHECK_EQ(store.seq(), 1ull);

    // The next contiguous delta applies; a multi-device one reports the rest.
    Delta next = delta.delta;
    next.seq = 2;
    next.source_kind = OriginKind::Scene;
    next.source_label.clear();
    next.changes[0].state.on.value = false;
    Change curtain;
    curtain.device_id = "living.curtain";
    curtain.state.position = {true, 0};
    next.changes.push_back(curtain);
    CHECK(store.ApplyDelta(next, 0) == ApplyOutcome::Applied);
    CHECK_EQ(DeviceStateText(Find(store, "living.curtain")), std::string("已关闭"));
    CHECK_EQ(FormatActivity(store.activity(), kChina), std::string("客厅空调 已关闭 等 2 个设备 · 来自 场景"));
    // Once a stale seq is below the held one it is a repeat even after a gap.
    Delta old = delta.delta;
    CHECK(store.ApplyDelta(old, kUtc) == ApplyOutcome::Ignored);

    // A delta brings a never-reported device its first state.
    Delta first_report = delta.delta;
    first_report.seq = 3;
    first_report.source_kind = OriginKind::Text;
    first_report.source_label.clear();
    first_report.changes[0].device_id = "entry.camera";
    first_report.changes[0].state = DeviceState{};
    first_report.changes[0].state.on = {true, true};
    CHECK(store.ApplyDelta(first_report, 0) == ApplyOutcome::Applied);
    CHECK(Find(store, "entry.camera").has_state);
    CHECK_EQ(DeviceStateText(Find(store, "entry.camera")), std::string("看护中"));
    CHECK_EQ(FormatActivity(store.activity(), kChina), std::string("摄像头 已上线 · 来自 文字"));

    SmartHomeStore empty;
    CHECK(empty.ApplyDelta(delta.delta, kUtc) == ApplyOutcome::NeedSync);
    CHECK(!empty.stale());  // nothing held, nothing to dim
}

void TestSnapshotReplacesAndLinkStaleness() {
    SmartHomeStore store = LoadedStore();
    const Message delta = Receive("panel-delta.json");
    CHECK(store.ApplyDelta(delta.delta, kUtc) == ApplyOutcome::Applied);

    store.SetLinkUp(false);
    CHECK(store.stale() && !store.link_up());
    CHECK(Find(store, "living.ac").state.on.value);  // last state kept
    store.SetLinkUp(true);
    CHECK(store.stale() && store.awaiting_snapshot());  // up, but deltas may be lost
    // A contiguous delta still applies, but only a snapshot clears staleness.
    Delta next = delta.delta;
    next.seq = 2;
    CHECK(store.ApplyDelta(next, kUtc) == ApplyOutcome::Applied);
    CHECK(store.stale());

    // A snapshot replaces everything, including revision, seq and activity.
    auto fresh = Receive("panel-snapshot.json");
    fresh.snapshot.revision = 5;
    fresh.snapshot.devices.resize(3);
    CHECK(store.ApplySnapshot(std::move(fresh.snapshot)) == ApplyOutcome::Applied);
    CHECK(!store.stale());
    CHECK_EQ(store.revision(), 5ull);
    CHECK_EQ(store.seq(), 0ull);
    CHECK_EQ(store.home().devices.size(), size_t{3});
    CHECK(store.FindDevice("living.ac") == nullptr);
    CHECK(!store.activity().valid);
    CHECK(store.ApplyDelta(delta.delta, kUtc) == ApplyOutcome::NeedSync);  // revision 1 != 5

    // Sync requests are throttled until a snapshot answers.
    SmartHomeStore throttle;
    CHECK(throttle.ClaimSyncRequest(1000));
    CHECK(!throttle.ClaimSyncRequest(1500));
    CHECK(throttle.ClaimSyncRequest(3000));
    auto answer = Receive("panel-snapshot.json");
    throttle.ApplySnapshot(std::move(answer.snapshot));
    CHECK(throttle.ClaimSyncRequest(3001));
    // A request sent before a drop died with the link.
    throttle.SetLinkUp(false);
    throttle.SetLinkUp(true);
    CHECK(throttle.ClaimSyncRequest(3002));
}

void TestVoiceResults() {
    const Message executed = Receive("voice-result-executed.json");
    CHECK(executed.kind == MessageKind::Result);
    CHECK(executed.result.outcome == VoiceOutcome::Executed);
    CHECK_EQ(executed.result.utterance, std::string("打开空调"));
    CHECK_EQ(executed.result.message, std::string("已打开客厅空调 · 26°C"));
    CHECK(executed.result.candidates.empty() && !executed.result.has_command);
    CHECK(OutcomeTone(executed.result.outcome) == ResultTone::Normal);

    const Message answered = Receive("voice-result-answered.json");
    CHECK(answered.result.outcome == VoiceOutcome::Answered);
    CHECK_EQ(answered.result.message, std::string("客厅空调开着，制冷 26°C"));
    CHECK(OutcomeTone(answered.result.outcome) == ResultTone::Normal);
    Message partial;
    CHECK(ParseMessage(kOpResult, Mutate(PayloadOf("voice-result-executed.json"), [](cJSON* root) {
        cJSON_ReplaceItemInObject(root, "outcome", cJSON_CreateString("partial"));
    }), partial));
    CHECK(partial.result.outcome == VoiceOutcome::Partial);
    CHECK(OutcomeTone(partial.result.outcome) == ResultTone::Attention);
    CHECK(OutcomeTone(VoiceOutcome::NotFound) == ResultTone::Error);
    CHECK(OutcomeTone(VoiceOutcome::Unavailable) == ResultTone::Error);

    const Message ambiguous = Receive("voice-result-ambiguous.json");
    const VoiceResult& r = ambiguous.result;
    CHECK(r.outcome == VoiceOutcome::Ambiguous && r.has_command);
    CHECK_EQ(r.turn_id, std::string("turn-golden-2"));
    CHECK_EQ(r.candidates.size(), size_t{2});
    CHECK_EQ(r.candidates[1].name, std::string("主卧空调"));
    CHECK_EQ(r.command.trait, std::string("on_off"));
    CHECK_EQ(r.command.command, std::string("on"));
    CHECK(r.command.params.empty());

    // Tapping a candidate runs the template on that device.
    Command chosen = r.command;
    chosen.device_id = r.candidates[0].device_id;
    CHECK(SameJson(BuildExecuteRequest("touch-00000001-1", {chosen}),
                   R"({"schema_v":1,"type":"smarthome.execute","payload":{"schema_version":1,)"
                   R"("request_id":"touch-00000001-1","commands":[)"
                   R"({"device_id":"living.ac","trait":"on_off","command":"on","params":{}}],"scene_id":null}})"));

    // Outcome and candidates must agree, both ways.
    const std::string payload = PayloadOf("voice-result-ambiguous.json");
    CHECK(Rejects(kOpResult, Mutate(payload, [](cJSON* root) {
        cJSON_DeleteItemFromArray(cJSON_GetObjectItem(root, "candidates"), 1);
    })));
    CHECK(Rejects(kOpResult, Mutate(payload, [](cJSON* root) {
        cJSON_ReplaceItemInObject(root, "command", cJSON_CreateNull());
    })));
    CHECK(Rejects(kOpResult, Mutate(PayloadOf("voice-result-executed.json"), [](cJSON* root) {
        cJSON_ReplaceItemInObject(root, "outcome", cJSON_CreateString("maybe"));
    })));
}

void TestUpwardBuildersMatchGoldens() {
    Command set;
    set.device_id = "living.main_light";
    set.trait = "level";
    set.command = "set";
    CommandParam value;
    value.name = "value";
    value.number = 40;
    set.params.push_back(value);
    CHECK(SameJson(BuildExecuteRequest("touch-golden-1", {set}), ReadGolden("panel-execute-request.json")));
    CHECK(SameJson(BuildSceneRequest("touch-golden-2", "scene.movie"), ReadGolden("panel-scene-request.json")));
    CHECK(SameJson(BuildSyncRequest(true, 1, 0), ReadGolden("panel-sync-request.json")));
    CHECK(SameJson(BuildSyncRequest(false, 0, 0),
                   R"({"schema_v":1,"type":"smarthome.sync","payload":)"
                   R"({"schema_version":1,"known_revision":null,"known_seq":null}})"));
    CHECK_EQ(std::string(kPanelRequestTopic), std::string("eidolon.smarthome"));

    // What cannot be expressed on the wire is not sent at all.
    CHECK(BuildExecuteRequest("touch golden", {set}).empty());
    CHECK(BuildExecuteRequest("touch-1", {}).empty());
    CHECK(BuildExecuteRequest("touch-1", std::vector<Command>(kMaxPanelCommands + 1, set)).empty());
    Command bad = set;
    bad.device_id.clear();
    CHECK(BuildExecuteRequest("touch-1", {bad}).empty());
    CHECK(BuildSceneRequest("touch-1", "").empty());
    CHECK(BuildSceneRequest("touch 1", "scene.movie").empty());

    RequestIdSource ids(0xabcd);
    const std::string first = ids.Next();
    CHECK_EQ(first, std::string("touch-0000abcd-1"));
    CHECK_EQ(ids.Next(), std::string("touch-0000abcd-2"));
    CHECK(IsIdentifier(first));
}

void TestContractViolationsAreRejected() {
    const std::string snapshot = PayloadOf("panel-snapshot.json");
    const std::string delta = PayloadOf("panel-delta.json");
    CHECK(Rejects("smarthome.unknown", snapshot));
    CHECK(Rejects(kOpSnapshot, "{not json"));
    CHECK(Rejects(kOpSnapshot, "[]"));
    CHECK(Rejects(kOpSnapshot, Mutate(snapshot, [](cJSON* root) {
        cJSON_ReplaceItemInObject(root, "schema_version", cJSON_CreateNumber(2));
    })));
    CHECK(Rejects(kOpSnapshot, Mutate(snapshot, [](cJSON* root) {
        cJSON* device = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "devices"), 0);
        cJSON_ReplaceItemInObject(device, "type", cJSON_CreateString("toaster"));
    })));
    CHECK(Rejects(kOpSnapshot, Mutate(snapshot, [](cJSON* root) {
        cJSON* device = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "devices"), 0);
        cJSON_ReplaceItemInObject(device, "area_id", cJSON_CreateString("garage"));
    })));
    CHECK(Rejects(kOpSnapshot, Mutate(snapshot, [](cJSON* root) {
        cJSON* device = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "devices"), 0);
        cJSON_ReplaceItemInObject(cJSON_GetObjectItem(device, "state"), "on", cJSON_CreateNumber(1));
    })));
    CHECK(Rejects(kOpSnapshot, Mutate(snapshot, [](cJSON* root) {
        cJSON* devices = cJSON_GetObjectItem(root, "devices");
        cJSON_AddItemToArray(devices, cJSON_Duplicate(cJSON_GetArrayItem(devices, 0), true));
    })));
    CHECK(Rejects(kOpSnapshot, Mutate(snapshot, [](cJSON* root) {
        cJSON_ReplaceItemInObject(root, "panel_area_id", cJSON_CreateString("garage"));
    })));
    CHECK(Rejects(kOpSnapshot, Mutate(snapshot, [](cJSON* root) {
        cJSON_ReplaceItemInObject(root, "revision", cJSON_CreateNumber(-1));
    })));
    CHECK(Rejects(kOpSnapshot, Mutate(snapshot, [](cJSON* root) {
        cJSON* device = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "devices"), 0);
        cJSON_ReplaceItemInObject(device, "state", cJSON_CreateNull());  // online needs state
    })));
    CHECK(Rejects(kOpSnapshot, Mutate(snapshot, [](cJSON* root) {
        cJSON* device = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "devices"), 0);
        cJSON_DeleteItemFromObject(device, "state");
    })));
    CHECK(Rejects(kOpSnapshot, Mutate(snapshot, [](cJSON* root) {
        cJSON_ReplaceItemInObject(root, "utc_offset_minutes", cJSON_CreateNumber(841));
    })));
    CHECK(Rejects(kOpSnapshot, Mutate(snapshot, [](cJSON* root) {
        cJSON_ReplaceItemInObject(root, "utc_offset_minutes", cJSON_CreateNumber(-721));
    })));
    CHECK(Rejects(kOpDelta, Mutate(delta, [](cJSON* root) {
        cJSON_ReplaceItemInObject(root, "seq", cJSON_CreateNumber(0));
    })));
    CHECK(Rejects(kOpDelta, Mutate(delta, [](cJSON* root) {
        cJSON_ReplaceItemInObject(root, "changes", cJSON_CreateArray());
    })));
    CHECK(Rejects(kOpDelta, Mutate(delta, [](cJSON* root) {
        cJSON_ReplaceItemInObject(cJSON_GetObjectItem(root, "source"), "kind", cJSON_CreateString("telepathy"));
    })));

    // Forward compatible where it costs nothing: unknown fields and a mode the
    // panel has no word for yet.
    Message message;
    CHECK(ParseMessage(kOpSnapshot, Mutate(snapshot, [](cJSON* root) {
        cJSON_AddStringToObject(root, "future_field", "x");
        cJSON* device = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "devices"), 3);
        cJSON_ReplaceItemInObject(cJSON_GetObjectItem(device, "state"), "mode", cJSON_CreateString("eco"));
    }), message));
    CHECK(message.snapshot.devices[3].state.mode == ThermostatMode::Unknown);
}

void TestTileTextTable() {
    SmartHomeStore store = LoadedStore();
    CHECK(store.ApplyDelta(Receive("panel-delta.json").delta, kUtc) == ApplyOutcome::Applied);
    const struct { const char* id; const char* glyph; const char* text; bool active; bool steps; } kRows[] = {
        {"living.main_light", "灯", "开 · 60%", true, true},
        {"master.light", "灯", "关", false, true},
        {"living.ac", "冷", "制冷 26°C · 室温 28°", true, true},
        {"master.ac", "冷", "关 · 室温 27°", false, true},
        {"living.curtain", "帘", "开 70%", true, true},
        {"balcony.rack", "晾", "已关闭", false, true},
        {"living.purifier", "净", "运行 · 30%", true, true},
        {"master.humidifier", "湿", "关", false, true},
        {"living.tv", "视", "关", false, true},
        {"living.speaker", "音", "开 · 音量 35", true, true},
        {"whole.vacuum", "扫", "空闲", false, false},
        {"balcony.washer", "洗", "空闲", false, false},
        {"kitchen.rice_cooker", "饭", "空闲", false, false},
        {"bath.water_heater", "热", "加热 · 42°C", true, true},
        {"entry.lock", "锁", "已上锁", false, false},
        {"entry.camera", "摄", "离线", false, false},  // offline, no state reported
        {"living.thermo", "温", "24.5°C · 48%", false, false},
    };
    for (const auto& row : kRows) {
        const Device& device = Find(store, row.id);
        CHECK_EQ(std::string(DeviceGlyph(device)), std::string(row.glyph));
        CHECK_EQ(DeviceStateText(device), std::string(row.text));
        CHECK_EQ(IsActive(device), row.active);
        CHECK_EQ(OffersSteps(device), row.steps);
    }

    DeviceState s;
    s.position = {true, 100};
    CHECK_EQ(DeviceStateText(Synthetic(DeviceType::Cover, s)), std::string("全开"));
    s = {};
    s.run_state = RunState::Running;
    CHECK_EQ(DeviceStateText(Synthetic(DeviceType::Appliance, s)), std::string("运行中"));
    CHECK(IsActive(Synthetic(DeviceType::Appliance, s)));
    s.run_state = RunState::Paused;
    CHECK_EQ(DeviceStateText(Synthetic(DeviceType::Appliance, s)), std::string("已暂停"));
    s.run_state = RunState::Docked;
    CHECK_EQ(DeviceStateText(Synthetic(DeviceType::Appliance, s)), std::string("已回充"));
    s = {};
    s.locked = {true, false};
    CHECK_EQ(DeviceStateText(Synthetic(DeviceType::Lock, s)), std::string("未上锁"));
    CHECK(IsActive(Synthetic(DeviceType::Lock, s)));
    s = {};
    s.on = {true, false};
    CHECK_EQ(DeviceStateText(Synthetic(DeviceType::Camera, s)), std::string("已关闭"));
    CHECK_EQ(DeviceStateText(Synthetic(DeviceType::Switch, s)), std::string("关"));
    s = {};
    s.on = {true, true};
    s.mode = ThermostatMode::Heat;
    s.target_c = {true, 24.5};
    CHECK_EQ(DeviceStateText(Synthetic(DeviceType::Climate, s)), std::string("制热 24.5°C"));
    CHECK_EQ(std::string(DeviceGlyph(Synthetic(DeviceType::Climate, s))), std::string("暖"));
    s = {};
    s.on = {true, true};
    s.volume = {true, 35};
    s.muted = {true, true};
    CHECK_EQ(DeviceStateText(Synthetic(DeviceType::Media, s)), std::string("开 · 静音"));
    s = {};
    s.temp_c = {true, 21};
    CHECK_EQ(DeviceStateText(Synthetic(DeviceType::Sensor, s)), std::string("21.0°C"));
    CHECK_EQ(DeviceStateText(Synthetic(DeviceType::Sensor, DeviceState{})), std::string("暂无读数"));
    Device offline = Find(store, "living.main_light");
    offline.online = false;
    CHECK_EQ(DeviceStateText(offline), std::string("离线"));
    CHECK(!IsActive(offline) && !OffersSteps(offline));
}

void TestTileCommandTable() {
    SmartHomeStore store = LoadedStore();
    const struct { const char* id; TileAction action; const char* expected; } kRows[] = {
        {"living.main_light", TileAction::Tap, "living.main_light on_off.toggle"},
        {"living.main_light", TileAction::Minus, "living.main_light level.step delta=-20"},
        {"living.main_light", TileAction::Plus, "living.main_light level.step delta=20"},
        {"living.ac", TileAction::Tap, "living.ac on_off.toggle"},
        {"living.ac", TileAction::Plus, "living.ac thermostat.step delta=1"},
        {"living.ac", TileAction::Minus, "living.ac thermostat.step delta=-1"},
        {"bath.water_heater", TileAction::Plus, "bath.water_heater thermostat.step delta=1"},
        {"living.curtain", TileAction::Tap, "living.curtain position.close"},
        {"living.curtain", TileAction::Plus, "living.curtain position.set value=90"},
        {"living.curtain", TileAction::Minus, "living.curtain position.set value=50"},
        {"balcony.rack", TileAction::Tap, "balcony.rack position.open"},
        {"balcony.rack", TileAction::Minus, "none"},  // already closed
        {"living.purifier", TileAction::Plus, "living.purifier fan_speed.set value=40"},
        {"living.purifier", TileAction::Minus, "living.purifier fan_speed.set value=20"},
        {"living.speaker", TileAction::Plus, "living.speaker volume.step delta=5"},
        {"living.speaker", TileAction::Minus, "living.speaker volume.step delta=-5"},
        {"whole.vacuum", TileAction::Tap, "whole.vacuum operational.start"},
        {"whole.vacuum", TileAction::Plus, "none"},
        {"entry.lock", TileAction::Tap, "entry.lock lock.unlock"},
        {"entry.camera", TileAction::Tap, "none"},  // offline
        {"living.thermo", TileAction::Tap, "none"},
    };
    for (const auto& row : kRows) CHECK_EQ(CommandFor(Find(store, row.id), row.action), std::string(row.expected));

    DeviceState s;
    s.run_state = RunState::Running;
    CHECK_EQ(CommandFor(Synthetic(DeviceType::Appliance, s), TileAction::Tap),
             std::string("test.device operational.pause"));
    s = {};
    s.locked = {true, false};
    CHECK_EQ(CommandFor(Synthetic(DeviceType::Lock, s), TileAction::Tap), std::string("test.device lock.lock"));
    s = {};
    s.position = {true, 100};
    CHECK_EQ(CommandFor(Synthetic(DeviceType::Cover, s), TileAction::Plus), std::string("none"));
    s = {};
    s.on = {true, true};
    s.speed = {true, 10};
    CHECK_EQ(CommandFor(Synthetic(DeviceType::Fan, s), TileAction::Minus), std::string("none"));
    CHECK_EQ(CommandFor(Synthetic(DeviceType::Cover, DeviceState{}), TileAction::Plus), std::string("none"));
    Device offline = Find(store, "living.main_light");
    offline.online = false;
    CHECK_EQ(CommandFor(offline, TileAction::Tap), std::string("none"));

    // A tile command is a well-formed PanelExecute.
    Command command;
    CHECK(TileCommand(Find(store, "living.main_light"), TileAction::Minus, command));
    CHECK(SameJson(BuildExecuteRequest("touch-00000001-7", {command}),
                   R"({"schema_v":1,"type":"smarthome.execute","payload":{"schema_version":1,)"
                   R"("request_id":"touch-00000001-7","commands":[{"device_id":"living.main_light",)"
                   R"("trait":"level","command":"step","params":{"delta":-20}}],"scene_id":null}})"));
}

void TestChangeWordingAndNavigation() {
    SmartHomeStore store = LoadedStore();
    const auto change = [&](const char* id, void (*edit)(Change&)) {
        const Device& device = Find(store, id);
        Change c;
        c.device_id = device.id;
        c.online = device.online;
        c.state = device.state;
        edit(c);
        return DescribeChange(device, c);
    };
    CHECK_EQ(change("living.main_light", [](Change& c) { c.state.on.value = false; }), std::string("已关闭"));
    CHECK_EQ(change("living.main_light", [](Change& c) { c.state.level.value = 40; }), std::string("调到 40%"));
    CHECK_EQ(change("living.ac", [](Change& c) { c.state.target_c.value = 24; }), std::string("设为 24°C"));
    CHECK_EQ(change("living.curtain", [](Change& c) { c.state.position.value = 30; }), std::string("开到 30%"));
    CHECK_EQ(change("entry.lock", [](Change& c) { c.state.locked.value = false; }), std::string("已开锁"));
    CHECK_EQ(change("whole.vacuum", [](Change& c) { c.state.run_state = RunState::Running; }),
             std::string("开始运行"));
    CHECK_EQ(change("living.speaker", [](Change& c) { c.state.volume.value = 50; }), std::string("音量 50"));
    CHECK_EQ(change("living.tv", [](Change& c) { c.online = false; }), std::string("离线"));
    CHECK_EQ(change("living.thermo", [](Change& c) { c.state.temp_c.value = 25.1; }),
             std::string("25.1°C · 48%"));

    const auto nav = BuildNav(store.home());
    CHECK_EQ(nav.size(), size_t{8});
    CHECK_EQ(nav[0].name, std::string("全部"));
    CHECK_EQ(nav[0].count, size_t{18});
    CHECK_EQ(nav[1].name, std::string("客厅"));
    CHECK_EQ(nav[1].count, size_t{7});
    CHECK_EQ(nav[2].count, size_t{4});
    CHECK_EQ(nav[7].name, std::string("全屋"));
    CHECK_EQ(DefaultArea(store.home()), std::string("living"));

    Snapshot unplaced = store.home();
    unplaced.panel_area_id.clear();
    unplaced.devices[0].area_id.clear();
    unplaced.areas.pop_back();  // "whole" no longer exists...
    unplaced.devices[10].area_id.clear();  // ...and its vacuum is placed nowhere
    const auto nav2 = BuildNav(unplaced);
    CHECK_EQ(nav2.back().name, std::string("其他"));
    CHECK_EQ(nav2.back().count, size_t{2});
    CHECK(InArea(unplaced.devices[0], kUnplacedArea) && !InArea(unplaced.devices[1], kUnplacedArea));
    CHECK_EQ(DefaultArea(unplaced), std::string(kAllAreas));

    CHECK_EQ(FormatClock(0, kChina), std::string("--:--"));
    CHECK_EQ(FormatClock(kUtc, 0), std::string("22:13"));
    CHECK_EQ(FormatClock(kUtc, kChina), std::string("06:13"));
    CHECK_EQ(FormatClock(kUtc, -23 * 60), std::string("23:13"));
}

}  // namespace

int main() {
    TestSnapshotGolden();
    TestDeltaGoldenAndSequence();
    TestSnapshotReplacesAndLinkStaleness();
    TestVoiceResults();
    TestUpwardBuildersMatchGoldens();
    TestContractViolationsAreRejected();
    TestTileTextTable();
    TestTileCommandTable();
    TestChangeWordingAndNavigation();
    if (g_failures != 0) {
        std::fprintf(stderr, "smarthome_store_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("smarthome_store_test: all passed\n");
    return 0;
}
