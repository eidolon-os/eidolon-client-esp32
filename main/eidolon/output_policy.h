#pragma once
#ifdef ESP_PLATFORM
#include <sdkconfig.h>
#endif
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
struct cJSON;
#include "eidolon/expression/generated/output_catalog.h"
namespace eidolon {
#if CONFIG_EIDOLON_COMPANION_FACE
inline constexpr bool kCompanionFaceBuild = true;
#else
inline constexpr bool kCompanionFaceBuild = false;
#endif
#if CONFIG_EIDOLON_OUTPUT_POLICY_V1 || CONFIG_EIDOLON_COMPANION_FACE
inline constexpr bool kOutputPolicyRequired = true;
#else
inline constexpr bool kOutputPolicyRequired = false;
#endif
constexpr uint32_t OutputBit(presentation::Output output) { return static_cast<uint32_t>(output); }
inline constexpr uint32_t kAudioOutputs = OutputBit(presentation::Output::Speech) |
    OutputBit(presentation::Output::AudioCue);
// How far the Owner lets the device use its camera (SDK CameraUse), each level
// including the one before. Unset: the policy says nothing, which never allows
// a still to leave the device.
enum class CameraUse : uint8_t { Unset, Off, Local, Capture };
struct DeviceOutputPolicy {
    bool known=false;
    uint32_t revision=0;
    uint32_t allowed=0;
    bool inputs_known=false;
    bool microphone=false;
    CameraUse camera=CameraUse::Unset;
    bool operator==(const DeviceOutputPolicy& other) const {
        return known==other.known && revision==other.revision && allowed==other.allowed &&
            inputs_known==other.inputs_known && microphone==other.microphone && camera==other.camera;
    }
};
struct SessionOutputPlan {
    std::string session_id;
    uint32_t policy_revision=0, selected=0;
    bool face_profile=false;
    bool microphone=true;
    CameraUse camera=CameraUse::Unset;
    // Installed device capabilities this session may invoke, as the Host's
    // plan lists them. Absent from the wire when there are none.
    std::vector<std::string> device_capabilities{};
};

// What an agent's invocation in the current session is bound to; empty
// `session_id` when no session is active.
struct SessionCapabilityBinding {
    std::string session_id;
    uint32_t policy_revision=0;
    std::vector<std::string> capabilities;
};
bool ParseOutputPolicy(const cJSON* json,DeviceOutputPolicy& output);
// The policy as ParseOutputPolicy reads it back, added to `parent` under
// `name`; nothing for an unknown policy. The device keeps it across reboots.
bool AddOutputPolicyJson(cJSON* parent,const char* name,const DeviceOutputPolicy& policy);
bool ParseSessionOutputPlan(const cJSON* json,SessionOutputPlan& output);

// No turn-taking state lives here. All consumers read one coherent output mask.
// Public transport callbacks authenticate the source before applying a plan.
class DeviceOutputGate {
public:
    explicit DeviceOutputGate(bool legacy_profile, uint32_t supported_outputs = UINT32_MAX)
        : legacy_(legacy_profile), supported_outputs_(supported_outputs) {}
    bool Bind(const DeviceOutputPolicy& policy);
    bool Start(const SessionOutputPlan& plan,const std::string& expected_session);
    // A transport interruption suspends execution, not the accepted grant.
    void Suspend();
    bool CanResume(const std::string& expected_session) const;
    bool Resume(const std::string& expected_session);
    void Close();
    bool Allows(presentation::Output output) const;
    bool AllowsAny(uint32_t outputs) const;
    bool AllowsMicrophone() const;
    // Whether the Owner's current policy lets one still leave the device on
    // request. Read when a still is asked for, so a policy narrowed during a
    // session refuses the next one even before the session is rebuilt.
    bool AllowsCameraCapture() const;
    // Local life outside a conversation (a pet glancing around). Inside an active
    // session the session plan decides, exactly like Allows(); an interrupted
    // session allows nothing; with no session the Owner's standing policy decides.
    bool AllowsAmbient(presentation::Output output) const;
    // The active, unsuspended session's binding for device capability calls.
    SessionCapabilityBinding CapabilityBinding() const;
private:
    bool CanResumeLocked(const std::string& expected_session) const;
    mutable std::mutex mutex_;
    bool legacy_=false, active_=false, suspended_=false;
    std::string session_id_;
    DeviceOutputPolicy policy_;
    uint32_t selected_=0;
    bool microphone_=false;
    std::vector<std::string> device_capabilities_;
    const uint32_t supported_outputs_;
};
DeviceOutputGate& CurrentOutputGate();
}
