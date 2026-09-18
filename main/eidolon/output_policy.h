#pragma once
#ifdef ESP_PLATFORM
#include <sdkconfig.h>
#endif
#include <cstdint>
#include <string>
#include <mutex>
struct cJSON;
#include "eidolon/expression/generated/output_catalog.h"
namespace eidolon {
#if CONFIG_EIDOLON_COMPANION_FACE
inline constexpr bool kCompanionFaceBuild = true;
#else
inline constexpr bool kCompanionFaceBuild = false;
#endif
constexpr uint32_t OutputBit(presentation::Output output) { return static_cast<uint32_t>(output); }
struct DeviceOutputPolicy {
    bool known=false;
    uint32_t revision=0;
    uint32_t allowed=0;
    bool operator==(const DeviceOutputPolicy& other) const {
        return known==other.known && revision==other.revision && allowed==other.allowed;
    }
};
struct SessionOutputPlan {
    std::string session_id;
    uint32_t policy_revision=0, selected=0;
    bool face_profile=false;
};
bool ParseOutputPolicy(const cJSON* json,DeviceOutputPolicy& output);
bool ParseSessionOutputPlan(const cJSON* json,SessionOutputPlan& output);

// No turn-taking state lives here. All consumers read one coherent output mask.
// Public transport callbacks authenticate the source before applying a plan.
class DeviceOutputGate {
public:
    explicit DeviceOutputGate(bool legacy_profile, uint32_t supported_outputs = UINT32_MAX)
        : legacy_(legacy_profile), supported_outputs_(supported_outputs) {}
    bool Bind(const DeviceOutputPolicy& policy);
    bool Start(const SessionOutputPlan& plan,const std::string& expected_session);
    void Close();
    bool Allows(presentation::Output output) const;
private:
    mutable std::mutex mutex_;
    bool legacy_=false, active_=false;
    DeviceOutputPolicy policy_;
    uint32_t selected_=0;
    const uint32_t supported_outputs_;
};
DeviceOutputGate& CurrentOutputGate();
}
