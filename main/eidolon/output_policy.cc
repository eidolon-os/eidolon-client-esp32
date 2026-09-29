#include "output_policy.h"
#include "device_capabilities.h"
#include <cJSON.h>
#include "eidolon/expression/generated/presentation_catalog.h"
#include <cmath>
#include <cstring>
#include <initializer_list>
#ifdef ESP_PLATFORM
#include <sdkconfig.h>
#endif
namespace eidolon {
namespace {
bool KeysWithOptionalInputs(const cJSON* obj) {
    if (!cJSON_IsObject(obj)) return false;
    for (auto* item=obj->child;item;item=item->next) {
        const bool known = item->string &&
            (!std::strcmp(item->string,"schema_version") ||
             !std::strcmp(item->string,"revision") ||
             !std::strcmp(item->string,"allowed") ||
             !std::strcmp(item->string,"inputs"));
        if (!known) return false;
        if (!std::strcmp(item->string,"inputs")) {
            if (cJSON_IsNull(item)) continue;
            if (!cJSON_IsObject(item) || cJSON_GetArraySize(item) != 1) return false;
            const auto* microphone = cJSON_GetObjectItemCaseSensitive(item, "microphone");
            if (!cJSON_IsBool(microphone)) return false;
        }
        for (auto* prior=obj->child;prior!=item;prior=prior->next)
            if (!std::strcmp(prior->string,item->string)) return false;
    }
    return true;
}
bool KeysWithSessionInputs(const cJSON* obj) {
    if (!cJSON_IsObject(obj)) return false;
    for (auto* item=obj->child;item;item=item->next) {
        const bool known = item->string &&
            (!std::strcmp(item->string,"schema_version") ||
             !std::strcmp(item->string,"session_id") ||
             !std::strcmp(item->string,"policy_revision") ||
             !std::strcmp(item->string,"outputs") ||
             !std::strcmp(item->string,"inputs") ||
             !std::strcmp(item->string,"expression_profile"));
        if (!known) return false;
        if (!std::strcmp(item->string,"inputs")) {
            if (!cJSON_IsObject(item) || cJSON_GetArraySize(item) != 1) return false;
            const auto* microphone = cJSON_GetObjectItemCaseSensitive(item, "microphone");
            if (!cJSON_IsBool(microphone)) return false;
        }
        for (auto* prior=obj->child;prior!=item;prior=prior->next)
            if (!std::strcmp(prior->string,item->string)) return false;
    }
    return true;
}
bool Uint(const cJSON* value,uint32_t& out) {
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) || value->valuedouble<1 ||
        value->valuedouble>UINT32_MAX || std::floor(value->valuedouble)!=value->valuedouble) return false;
    out=static_cast<uint32_t>(value->valuedouble);return true;
}
bool Version(const cJSON* obj) {
    auto* value=cJSON_GetObjectItemCaseSensitive(obj,"schema_version");
    uint32_t version=1;
    return !value || (Uint(value,version) && version==1);
}
bool Mask(const cJSON* obj,uint32_t& out) {
    if (!cJSON_IsObject(obj)) return false;
    out=0;
    for (auto* item=obj->child;item;item=item->next) {
        if (!cJSON_IsBool(item) || !item->string) return false;
        size_t index=0;
        while (index<presentation::kOutputNames.size() && presentation::kOutputNames[index]!=item->string) ++index;
        if (index==presentation::kOutputNames.size()) return false;
        for (auto* prior=obj->child;prior!=item;prior=prior->next)
            if (!std::strcmp(prior->string,item->string)) return false;
        if (cJSON_IsTrue(item)) out|=1u<<index;
    }
    return true;
}
constexpr uint32_t kLegacyOutputs=OutputBit(presentation::Output::Speech)|
    OutputBit(presentation::Output::DialogueText)|OutputBit(presentation::Output::AudioCue);
constexpr uint32_t kResponseOutputs=OutputBit(presentation::Output::Speech)|
    OutputBit(presentation::Output::DialogueText)|OutputBit(presentation::Output::Expression);
}
bool ParseOutputPolicy(const cJSON* json,DeviceOutputPolicy& output) {
    if (!json || cJSON_IsNull(json)) { output={};return true; }
    DeviceOutputPolicy candidate;
    if (!KeysWithOptionalInputs(json) || !Version(json) ||
        !Uint(cJSON_GetObjectItemCaseSensitive(json,"revision"),candidate.revision) ||
        !Mask(cJSON_GetObjectItemCaseSensitive(json,"allowed"),candidate.allowed)) return false;
    candidate.known=true;output=candidate;return true;
}
bool ParseSessionOutputPlan(const cJSON* json,SessionOutputPlan& output) {
    if (!KeysWithSessionInputs(json) ||
        !Version(json)) return false;
    SessionOutputPlan plan;
    auto* session=cJSON_GetObjectItemCaseSensitive(json,"session_id");
    if (!cJSON_IsString(session) || !session->valuestring) return false;
    plan.session_id=session->valuestring;
    if (plan.session_id.empty() || plan.session_id.size()>96) return false;
    for (char c:plan.session_id) if (!(c>='a' && c<='z') && !(c>='A' && c<='Z') &&
        !(c>='0' && c<='9') && c!='-' && c!='_' && c!='.' && c!=':') return false;
    if (!Uint(cJSON_GetObjectItemCaseSensitive(json,"policy_revision"),plan.policy_revision) ||
        !Mask(cJSON_GetObjectItemCaseSensitive(json,"outputs"),plan.selected) || !(plan.selected&kResponseOutputs)) return false;
    auto* profile=cJSON_GetObjectItemCaseSensitive(json,"expression_profile");
    plan.face_profile=cJSON_IsString(profile) && !std::strcmp(profile->valuestring,expression::kProfile);
    if ((profile && !cJSON_IsNull(profile) && !plan.face_profile) ||
        plan.face_profile!=bool(plan.selected&OutputBit(presentation::Output::Expression))) return false;
    output=std::move(plan);return true;
}
bool DeviceOutputGate::Bind(const DeviceOutputPolicy& policy) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (policy_.known && (!policy.known || policy.revision<policy_.revision ||
        (policy.revision==policy_.revision && !(policy==policy_)))) {
        active_=false;selected_=0;return false;
    }
    if (!(policy==policy_)) { active_=false;selected_=0; }
    policy_=policy;return true;
}
bool DeviceOutputGate::Start(const SessionOutputPlan& plan,const std::string& expected_session) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!policy_.known || plan.session_id!=expected_session || plan.policy_revision!=policy_.revision ||
        !(plan.selected&kResponseOutputs) || (plan.selected&~policy_.allowed) ||
        (plan.selected&~supported_outputs_) ||
        plan.face_profile!=bool(plan.selected&OutputBit(presentation::Output::Expression))) {
        active_=false;selected_=0;return false;
    }
    active_=true;selected_=plan.selected;return true;
}
void DeviceOutputGate::Close() {
    std::lock_guard<std::mutex> lock(mutex_);active_=false;selected_=0;
}
bool DeviceOutputGate::Allows(presentation::Output output) const {
    return AllowsAny(OutputBit(output));
}
bool DeviceOutputGate::AllowsAny(uint32_t outputs) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto mask=policy_.known ? (active_ ? selected_ : 0u) : (legacy_ ? kLegacyOutputs : 0u);
    return mask & supported_outputs_ & outputs;
}
DeviceOutputGate& CurrentOutputGate() {
    static DeviceOutputGate gate(!kOutputPolicyRequired, CompiledDeviceCapabilities().OutputMask());
    return gate;
}
}
