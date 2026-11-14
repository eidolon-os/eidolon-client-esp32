#include "eidolon/device_settings_core.h"
#include "eidolon/generated/device_settings_catalog.h"

#include <cJSON.h>

#undef NDEBUG
#include <cassert>
#include <cstring>
#include <set>
#include <string>

using namespace eidolon::device_settings;

namespace {

struct Board {
    int volume = 40;
    int brightness = 70;
    int volume_applies = 0;
    int brightness_applies = 0;

    Actuators actuators(bool speaker = true, bool display = true)
    {
        Actuators value;
        if (speaker) {
            value.read_volume = [this] { return std::optional<int>(volume); };
            value.apply_volume = [this](int target) {
                ++volume_applies;
                volume = target;
                return std::optional<int>(volume);
            };
        }
        if (display) {
            value.read_brightness = [this] { return std::optional<int>(brightness); };
            value.apply_brightness = [this](int target) {
                ++brightness_applies;
                brightness = target;
                return std::optional<int>(brightness);
            };
        }
        return value;
    }
};

constexpr int64_t kNow = 1'800'000'000'000;

Binding Standing()
{
    Binding binding;
    binding.room = "eidolon-0123abcd";
    binding.standing_policy_revision = 7;
    return binding;
}

Binding InSession(std::initializer_list<const char*> capabilities)
{
    Binding binding = Standing();
    binding.session_id = "conversation-1";
    binding.session_policy_revision = 7;
    for (const char* name : capabilities) binding.session_capabilities.emplace_back(name);
    return binding;
}

std::string Invocation(const char* execution_id, const char* session_id, int revision,
                       const char* capability, const char* arguments,
                       int64_t expires_at_ms = kNow + 10'000)
{
    return std::string("{\"execution_id\":\"") + execution_id + "\",\"session_id\":\"" +
        session_id + "\",\"policy_revision\":" + std::to_string(revision) +
        ",\"protocol_version\":1,\"capability\":\"" + capability +
        "\",\"capability_version\":1,\"arguments\":" + arguments +
        ",\"expires_at_ms\":" + std::to_string(expires_at_ms) + ",\"max_duration_ms\":5000}";
}

cJSON* Parse(const std::string& text)
{
    cJSON* json = cJSON_Parse(text.c_str());
    assert(json != nullptr);
    return json;
}

std::string Field(const std::string& snapshot, const char* key)
{
    cJSON* json = Parse(snapshot);
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(json, key);
    std::string out;
    if (cJSON_IsString(value)) out = value->valuestring;
    cJSON_Delete(json);
    return out;
}

int ResultValue(const std::string& snapshot, const char* key)
{
    cJSON* json = Parse(snapshot);
    const cJSON* result = cJSON_GetObjectItemCaseSensitive(json, "result");
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(result, key);
    assert(cJSON_IsNumber(value));
    const int out = value->valueint;
    cJSON_Delete(json);
    return out;
}

// The bounds this module enforces must be the ones the embedded contract
// declares, or a value the Host validated could be refused here.
void BoundsAgreeWithTheContract()
{
    const std::string actions = std::string("[") + kSpeakerActions + "," + kDisplayActions + "]";
    cJSON* json = Parse(actions);
    std::set<std::string> names;
    for (const cJSON* action = json->child; action != nullptr; action = action->next) {
        const std::string name = cJSON_GetObjectItemCaseSensitive(action, "name")->valuestring;
        names.insert(name);
        const cJSON* properties = cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(action, "input_schema"), "properties");
        const cJSON* field = properties->child;
        assert(field != nullptr && field->next == nullptr);
        const bool adjust = name.size() > 7 && name.compare(name.size() - 7, 7, ".adjust") == 0;
        assert(std::strcmp(field->string, adjust ? "change" : (name == kVolumeSet ? "volume" : "brightness")) == 0);
        assert(cJSON_GetObjectItemCaseSensitive(field, "minimum")->valueint == (adjust ? -100 : 0));
        assert(cJSON_GetObjectItemCaseSensitive(field, "maximum")->valueint == 100);
        assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(action, "idempotent")));
    }
    assert((names == std::set<std::string>{kVolumeSet, kVolumeAdjust, kBrightnessSet, kBrightnessAdjust}));
    cJSON_Delete(json);
}

void DeclaresOnlyWhatTheBoardHas()
{
    assert(DeclaredActions(true, true) == std::string(kSpeakerActions) + "," + kDisplayActions);
    assert(DeclaredProperties(true, true) == std::string(kSpeakerProperties) + "," + kDisplayProperties);
    assert(DeclaredActions(true, false) == kSpeakerActions);
    assert(DeclaredActions(false, true) == kDisplayActions);
    assert(DeclaredProperties(false, true) == kDisplayProperties);
    assert(DeclaredActions(false, false).empty() && DeclaredProperties(false, false).empty());
    Board board;
    assert(Runtime(board.actuators()).HasSpeaker() && Runtime(board.actuators()).HasDisplay());
    assert(!Runtime(board.actuators(false, false)).HasSpeaker());
}

void TheOwnersAppSetsOnTheStandingChannel()
{
    Board board;
    Runtime runtime(board.actuators());
    const auto answer = runtime.Handle(kInvokeMethod, Caller::Provider,
        Invocation("owner:1", "eidolon-0123abcd", 7, kVolumeSet, R"({"volume":30})"),
        Standing(), kNow, true);
    assert(answer.ok);
    assert(Field(answer.payload, "state") == "completed");
    assert(Field(answer.payload, "completion_basis") == "configuration_applied");
    assert(ResultValue(answer.payload, "volume") == 30);
    assert(board.volume == 30);

    // The snapshot is exactly the SDK's field set.
    cJSON* json = Parse(answer.payload);
    std::set<std::string> keys;
    for (const cJSON* item = json->child; item != nullptr; item = item->next) keys.insert(item->string);
    assert((keys == std::set<std::string>{"execution_id", "state", "sequence", "result",
                                          "completion_basis", "error"}));
    cJSON_Delete(json);
}

void TheStandingChannelIsBoundToItsRoomAndPolicy()
{
    Board board;
    Runtime runtime(board.actuators());
    auto other_room = runtime.Handle(kInvokeMethod, Caller::Provider,
        Invocation("owner:1", "eidolon-other", 7, kVolumeSet, R"({"volume":30})"), Standing(), kNow, true);
    assert(!other_room.ok && other_room.error_code == kRpcInvalidOrInactive);
    auto stale_policy = runtime.Handle(kInvokeMethod, Caller::Provider,
        Invocation("owner:2", "eidolon-0123abcd", 6, kVolumeSet, R"({"volume":30})"), Standing(), kNow, true);
    assert(!stale_policy.ok && stale_policy.error_code == kRpcInvalidOrInactive);
    Binding no_policy = Standing();
    no_policy.standing_policy_revision = 0;
    auto unbound = runtime.Handle(kInvokeMethod, Caller::Provider,
        Invocation("owner:3", "eidolon-0123abcd", 7, kVolumeSet, R"({"volume":30})"), no_policy, kNow, true);
    assert(!unbound.ok && unbound.error_code == kRpcInvalidOrInactive);
    auto stranger = runtime.Handle(kInvokeMethod, Caller::Unknown,
        Invocation("owner:4", "eidolon-0123abcd", 7, kVolumeSet, R"({"volume":30})"), Standing(), kNow, true);
    assert(!stranger.ok && stranger.error_code == kRpcUnauthorized);
    // The Owner's app reaches only the setters.
    auto adjust = runtime.Handle(kInvokeMethod, Caller::Provider,
        Invocation("owner:5", "eidolon-0123abcd", 7, kVolumeAdjust, R"({"change":10})"), Standing(), kNow, true);
    assert(adjust.ok && Field(adjust.payload, "error") == "unsupported_capability");
    assert(board.volume_applies == 0);
}

void AnAgentActsOnlyWithinItsSessionPlan()
{
    Board board;
    board.volume = 95;
    Runtime runtime(board.actuators());
    const Binding session = InSession({kVolumeAdjust});
    auto louder = runtime.Handle(kInvokeMethod, Caller::Agent,
        Invocation("exec:1", "conversation-1", 7, kVolumeAdjust, R"({"change":10})"), session, kNow, true);
    assert(louder.ok && ResultValue(louder.payload, "volume") == 100);  // clamped
    auto not_planned = runtime.Handle(kInvokeMethod, Caller::Agent,
        Invocation("exec:2", "conversation-1", 7, kBrightnessSet, R"({"brightness":10})"), session, kNow, true);
    assert(not_planned.ok && Field(not_planned.payload, "error") == "unsupported_capability");
    auto other_session = runtime.Handle(kInvokeMethod, Caller::Agent,
        Invocation("exec:3", "conversation-2", 7, kVolumeAdjust, R"({"change":10})"), session, kNow, true);
    assert(!other_session.ok && other_session.error_code == kRpcInvalidOrInactive);
    auto idle = runtime.Handle(kInvokeMethod, Caller::Agent,
        Invocation("exec:4", "conversation-1", 7, kVolumeAdjust, R"({"change":10})"), Standing(), kNow, true);
    assert(!idle.ok && idle.error_code == kRpcInvalidOrInactive);
    // A capability the board does not have is not one a plan can grant.
    Runtime silent(board.actuators(true, false));
    auto no_display = silent.Handle(kInvokeMethod, Caller::Agent,
        Invocation("exec:5", "conversation-1", 7, kBrightnessSet, R"({"brightness":10})"),
        InSession({kBrightnessSet}), kNow, true);
    assert(no_display.ok && Field(no_display.payload, "error") == "unsupported_capability");
}

void ARetryIsTheSameExecution()
{
    Board board;
    Runtime runtime(board.actuators());
    const std::string request =
        Invocation("owner:retry", "eidolon-0123abcd", 7, kBrightnessSet, R"({"brightness":20})");
    const auto first = runtime.Handle(kInvokeMethod, Caller::Provider, request, Standing(), kNow, true);
    const auto again = runtime.Handle(kInvokeMethod, Caller::Provider, request, Standing(), kNow, true);
    assert(first.ok && again.ok && first.payload == again.payload);
    assert(board.brightness_applies == 1);
    const auto changed = runtime.Handle(kInvokeMethod, Caller::Provider,
        Invocation("owner:retry", "eidolon-0123abcd", 7, kBrightnessSet, R"({"brightness":90})"),
        Standing(), kNow, true);
    assert(changed.ok && Field(changed.payload, "error") == "id_conflict");
    assert(board.brightness == 20);

    // Status and cancel answer with the outcome already reached.
    const std::string scope = R"({"execution_id":"owner:retry","session_id":"eidolon-0123abcd","policy_revision":7})";
    const auto status = runtime.Handle(kStatusMethod, Caller::Provider, scope, Standing(), kNow, true);
    const auto cancel = runtime.Handle(kCancelMethod, Caller::Provider, scope, Standing(), kNow, true);
    assert(status.ok && status.payload == first.payload && cancel.payload == first.payload);
    const auto unknown = runtime.Handle(kStatusMethod, Caller::Provider,
        R"({"execution_id":"owner:none","session_id":"eidolon-0123abcd","policy_revision":7})",
        Standing(), kNow, true);
    assert(!unknown.ok && unknown.error_code == kRpcInvalidOrInactive);
    const auto other_scope = runtime.Handle(kStatusMethod, Caller::Agent, scope, InSession({}), kNow, true);
    assert(!other_scope.ok && other_scope.error_code == kRpcInvalidOrInactive);
}

void TheLedgerIsBounded()
{
    Board board;
    Runtime runtime(board.actuators());
    for (size_t i = 0; i <= kLedgerSize; ++i) {
        const std::string id = "owner:" + std::to_string(i);
        assert(runtime.Handle(kInvokeMethod, Caller::Provider,
            Invocation(id.c_str(), "eidolon-0123abcd", 7, kVolumeSet, R"({"volume":10})"),
            Standing(), kNow, true).ok);
    }
    // The oldest was overwritten, so asking about it is asking about nothing.
    const auto oldest = runtime.Handle(kStatusMethod, Caller::Provider,
        R"({"execution_id":"owner:0","session_id":"eidolon-0123abcd","policy_revision":7})",
        Standing(), kNow, true);
    assert(!oldest.ok);
}

void DeadlinesAndArguments()
{
    Board board;
    Runtime runtime(board.actuators());
    auto late = runtime.Handle(kInvokeMethod, Caller::Provider,
        Invocation("owner:late", "eidolon-0123abcd", 7, kVolumeSet, R"({"volume":5})", kNow - 1),
        Standing(), kNow, true);
    assert(late.ok && Field(late.payload, "error") == "expired");
    // A board that has not reached a time source cannot judge a deadline, and
    // its own volume still changes.
    auto unset_clock = runtime.Handle(kInvokeMethod, Caller::Provider,
        Invocation("owner:clock", "eidolon-0123abcd", 7, kVolumeSet, R"({"volume":5})", kNow - 1),
        Standing(), 12'345, false);
    assert(unset_clock.ok && Field(unset_clock.payload, "state") == "completed");
    for (const char* bad : {R"({"volume":101})", R"({"volume":-1})", R"({"volume":5.5})",
                            R"({"volume":"5"})", R"({})", R"({"volume":5,"extra":1})"}) {
        const std::string id = std::string("owner:bad-") + std::to_string(std::strlen(bad));
        auto refused = runtime.Handle(kInvokeMethod, Caller::Provider,
            Invocation(id.c_str(), "eidolon-0123abcd", 7, kVolumeSet, bad), Standing(), kNow, true);
        assert(refused.ok && Field(refused.payload, "error") == "invalid_arguments");
    }
    auto malformed = runtime.Handle(kInvokeMethod, Caller::Provider, "{not json", Standing(), kNow, true);
    assert(!malformed.ok && malformed.error_code == kRpcInvalidOrInactive);
    auto extra_key = runtime.Handle(kInvokeMethod, Caller::Provider,
        R"({"execution_id":"x","session_id":"eidolon-0123abcd","policy_revision":7,"protocol_version":1,"capability":"audio.volume.set","capability_version":1,"arguments":{"volume":1},"expires_at_ms":1800000010000,"max_duration_ms":5000,"surprise":true})",
        Standing(), kNow, true);
    assert(!extra_key.ok);
    auto oversized = runtime.Handle(kInvokeMethod, Caller::Provider,
        std::string(kMaxRpcPayloadBytes + 1, ' '), Standing(), kNow, true);
    assert(!oversized.ok);
}

void PropertiesAreReadAndReported()
{
    Board board;
    Runtime runtime(board.actuators());
    const auto read = runtime.Handle(kPropertiesReadMethod, Caller::Provider, "{}", Standing(), kNow, true);
    assert(read.ok && read.payload == R"({"values":{"audio.volume":40,"display.brightness":70}})");
    assert(runtime.Handle(kPropertiesReadMethod, Caller::Agent, "{}", InSession({}), kNow, true).ok);
    assert(!runtime.Handle(kPropertiesReadMethod, Caller::Unknown, "{}", Standing(), kNow, true).ok);
    Runtime speaker_only(board.actuators(true, false));
    assert(speaker_only.Handle(kPropertiesReadMethod, Caller::Provider, "{}", Standing(), kNow, true).payload ==
           R"({"values":{"audio.volume":40}})");

    assert(runtime.PropertyChangedPayload(kVolumeProperty, 33) == R"({"name":"audio.volume","value":33})");
    assert(!runtime.PropertyChangedPayload(kVolumeProperty, 101).has_value());
    assert(!runtime.PropertyChangedPayload("battery.level", 3).has_value());
    assert(!speaker_only.PropertyChangedPayload(kBrightnessProperty, 3).has_value());
    assert(Runtime::IsMethod(kInvokeMethod) && Runtime::IsMethod(kPropertiesReadMethod));
    assert(!Runtime::IsMethod("eidolon.capability.invoke.v2"));
}

}  // namespace

int main()
{
    BoundsAgreeWithTheContract();
    DeclaresOnlyWhatTheBoardHas();
    TheOwnersAppSetsOnTheStandingChannel();
    TheStandingChannelIsBoundToItsRoomAndPolicy();
    AnAgentActsOnlyWithinItsSessionPlan();
    ARetryIsTheSameExecution();
    TheLedgerIsBounded();
    DeadlinesAndArguments();
    PropertiesAreReadAndReported();
    return 0;
}
