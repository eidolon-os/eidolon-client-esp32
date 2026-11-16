#include "output_policy.h"
#include "device_capabilities.h"
#include <cJSON.h>
#include "eidolon/expression/generated/presentation_catalog.h"
#include "eidolon/generated/camera_catalog.h"
#include <cmath>
#include <cstring>
#include <initializer_list>
#ifdef ESP_PLATFORM
#include <sdkconfig.h>
#endif
namespace eidolon {
namespace {
bool Keys(const cJSON* obj,std::initializer_list<const char*> keys) {
    if (!cJSON_IsObject(obj)) return false;
    for (auto* item=obj->child;item;item=item->next) {
        bool found=false;
        for (auto* key:keys) if (item->string && !std::strcmp(item->string,key)) found=true;
        if (!found) return false;
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
// One of the SDK's camera uses; null is the SDK's "not decided" and reads as unset.
bool Camera(const cJSON* value,CameraUse& camera) {
    camera=CameraUse::Unset;
    if (!value || cJSON_IsNull(value)) return true;
    if (!cJSON_IsString(value) || !value->valuestring) return false;
    static_assert(camera_capture::kCameraUses.size()==3);
    static constexpr CameraUse kUses[]={CameraUse::Off,CameraUse::Local,CameraUse::Capture};
    for (size_t index=0;index<camera_capture::kCameraUses.size();++index) {
        if (camera_capture::kCameraUses[index]==value->valuestring) { camera=kUses[index];return true; }
    }
    return false;
}
bool Input(const cJSON* json,bool& microphone,CameraUse& camera) {
    if (!Keys(json,{"microphone","camera"})) return false;
    const auto* value=cJSON_GetObjectItemCaseSensitive(json,"microphone");
    if (value && !cJSON_IsBool(value)) return false;
    if (!Camera(cJSON_GetObjectItemCaseSensitive(json,"camera"),camera)) return false;
    microphone=cJSON_IsTrue(value);return true;
}
}
bool ParseOutputPolicy(const cJSON* json,DeviceOutputPolicy& output) {
    if (!json || cJSON_IsNull(json)) { output={};return true; }
    DeviceOutputPolicy candidate;
    if (!Keys(json,{"schema_version","revision","allowed","inputs"}) || !Version(json) ||
        !Uint(cJSON_GetObjectItemCaseSensitive(json,"revision"),candidate.revision) ||
        !Mask(cJSON_GetObjectItemCaseSensitive(json,"allowed"),candidate.allowed)) return false;
    const auto* inputs=cJSON_GetObjectItemCaseSensitive(json,"inputs");
    if (inputs && !cJSON_IsNull(inputs)) {
        if (!Input(inputs,candidate.microphone,candidate.camera)) return false;
        candidate.inputs_known=true;
    }
    candidate.known=true;output=candidate;return true;
}
bool AddOutputPolicyJson(cJSON* parent,const char* name,const DeviceOutputPolicy& policy) {
    if (!policy.known) return true;
    auto* json=cJSON_AddObjectToObject(parent,name);
    if (!json) return false;
    cJSON_AddNumberToObject(json,"schema_version",1);
    cJSON_AddNumberToObject(json,"revision",policy.revision);
    if (policy.inputs_known) {
        auto* inputs=cJSON_AddObjectToObject(json,"inputs");
        if (!inputs) return false;
        cJSON_AddBoolToObject(inputs,"microphone",policy.microphone);
        if (policy.camera!=CameraUse::Unset) {
            const auto use=camera_capture::kCameraUses[static_cast<size_t>(policy.camera)-1];
            cJSON_AddStringToObject(inputs,"camera",std::string(use).c_str());
        }
    }
    auto* allowed=cJSON_AddObjectToObject(json,"allowed");
    if (!allowed) return false;
    for (size_t i=0;i<presentation::kOutputNames.size();++i)
        cJSON_AddBoolToObject(allowed,presentation::kOutputNames[i].data(),policy.allowed&(1u<<i));
    return true;
}
bool ParseSessionOutputPlan(const cJSON* json,SessionOutputPlan& output) {
    if (!Keys(json,{"schema_version","session_id","policy_revision","outputs","expression_profile","motion_profile","inputs","device_capabilities"}) ||
        !Version(json)) return false;
    SessionOutputPlan plan;
    if (const auto* capabilities=cJSON_GetObjectItemCaseSensitive(json,"device_capabilities")) {
        // The SDK bounds a session's set at 64 names; each is a registry name.
        if (!cJSON_IsArray(capabilities) || cJSON_GetArraySize(capabilities)>64) return false;
        const cJSON* name=nullptr;
        cJSON_ArrayForEach(name,capabilities) {
            if (!cJSON_IsString(name) || !name->valuestring || !*name->valuestring ||
                std::strlen(name->valuestring)>128) return false;
            plan.device_capabilities.emplace_back(name->valuestring);
        }
    }
    const auto* inputs=cJSON_GetObjectItemCaseSensitive(json,"inputs");
    if (inputs && !Input(inputs,plan.microphone,plan.camera)) return false;
    auto* session=cJSON_GetObjectItemCaseSensitive(json,"session_id");
    if (!cJSON_IsString(session) || !session->valuestring) return false;
    plan.session_id=session->valuestring;
    if (plan.session_id.empty() || plan.session_id.size()>96) return false;
    for (char c:plan.session_id) if (!(c>='a' && c<='z') && !(c>='A' && c<='Z') &&
        !(c>='0' && c<='9') && c!='-' && c!='_' && c!='.' && c!=':') return false;
    if (!Uint(cJSON_GetObjectItemCaseSensitive(json,"policy_revision"),plan.policy_revision) ||
        !Mask(cJSON_GetObjectItemCaseSensitive(json,"outputs"),plan.selected) ||
        (!plan.microphone && !(plan.selected&kResponseOutputs))) return false;
    auto* profile=cJSON_GetObjectItemCaseSensitive(json,"expression_profile");
    plan.face_profile=cJSON_IsString(profile) && !std::strcmp(profile->valuestring,expression::kProfile);
    if ((profile && !cJSON_IsNull(profile) && !plan.face_profile) ||
        plan.face_profile!=bool(plan.selected&OutputBit(presentation::Output::Expression))) return false;
    auto* motion_profile=cJSON_GetObjectItemCaseSensitive(json,"motion_profile");
    if (motion_profile && !cJSON_IsNull(motion_profile) &&
        (!cJSON_IsString(motion_profile) || std::strcmp(motion_profile->valuestring,"stackchan.head.v1"))) return false;
    output=std::move(plan);return true;
}
bool DeviceOutputGate::Bind(const DeviceOutputPolicy& policy) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (policy_.known && (!policy.known || policy.revision<policy_.revision ||
        (policy.revision==policy_.revision && !(policy==policy_)))) {
        active_=false;selected_=0;session_id_.clear();device_capabilities_.clear();return false;
    }
    if (!(policy==policy_)) { active_=false;selected_=0;session_id_.clear();device_capabilities_.clear(); }
    policy_=policy;return true;
}
bool DeviceOutputGate::Start(const SessionOutputPlan& plan,const std::string& expected_session) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!policy_.known || plan.session_id!=expected_session || plan.policy_revision!=policy_.revision ||
        (plan.microphone && policy_.inputs_known && !policy_.microphone) ||
        (!plan.microphone && !(plan.selected&kResponseOutputs)) || (plan.selected&~policy_.allowed) ||
        (plan.selected&~supported_outputs_) ||
        plan.face_profile!=bool(plan.selected&OutputBit(presentation::Output::Expression))) {
        active_=false;selected_=0;session_id_.clear();device_capabilities_.clear();return false;
    }
    active_=true;suspended_=false;session_id_=plan.session_id;
    selected_=plan.selected;microphone_=plan.microphone;
    device_capabilities_=plan.device_capabilities;return true;
}
void DeviceOutputGate::Suspend() {
    std::lock_guard<std::mutex> lock(mutex_);suspended_=true;
}
bool DeviceOutputGate::CanResumeLocked(const std::string& expected_session) const {
    // Bind invalidates grants on every policy change/rejection. Legacy devices
    // have no server plan; the controller owns their session confirmation.
    return !expected_session.empty() &&
        (policy_.known ? (active_ && session_id_==expected_session) : legacy_);
}
bool DeviceOutputGate::CanResume(const std::string& expected_session) const {
    std::lock_guard<std::mutex> lock(mutex_);return CanResumeLocked(expected_session);
}
bool DeviceOutputGate::Resume(const std::string& expected_session) {
    std::lock_guard<std::mutex> lock(mutex_);
    suspended_=!CanResumeLocked(expected_session);
    return !suspended_;
}
void DeviceOutputGate::Close() {
    std::lock_guard<std::mutex> lock(mutex_);active_=false;suspended_=true;selected_=0;session_id_.clear();
    device_capabilities_.clear();
}
SessionCapabilityBinding DeviceOutputGate::CapabilityBinding() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!policy_.known || !active_ || suspended_) return {};
    return {session_id_, policy_.revision, device_capabilities_};
}
bool DeviceOutputGate::Allows(presentation::Output output) const {
    return AllowsAny(OutputBit(output));
}
bool DeviceOutputGate::AllowsMicrophone() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !suspended_ && (policy_.known ? (active_ && microphone_ &&
        (!policy_.inputs_known || policy_.microphone)) : legacy_);
}
bool DeviceOutputGate::AllowsCameraCapture() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return policy_.known && policy_.camera==CameraUse::Capture;
}
bool DeviceOutputGate::AllowsAny(uint32_t outputs) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto mask=policy_.known ? (active_ ? selected_ : 0u) : (legacy_ ? kLegacyOutputs : 0u);
    return !suspended_ && (mask & supported_outputs_ & outputs);
}
bool DeviceOutputGate::AllowsAmbient(presentation::Output output) const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t mask=0;
    if (!policy_.known) mask=(legacy_ && !suspended_) ? kLegacyOutputs : 0u;   // same as Allows()
    else if (active_) mask=suspended_ ? 0u : selected_;   // Close() clears active_, so suspended here = interrupted
    else mask=policy_.allowed;
    return (mask & supported_outputs_ & OutputBit(output)) != 0;
}
DeviceOutputGate& CurrentOutputGate() {
    static DeviceOutputGate gate(!kOutputPolicyRequired, CompiledDeviceCapabilities().OutputMask());
    return gate;
}
}
