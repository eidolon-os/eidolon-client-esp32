#!/usr/bin/env python3
"""Exercise production snapshot application and audio gates, isolating HTTP/NVS."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]


def definition(text, needle):
    begin = text.index(needle)
    end = text.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[begin:end]


source = (root / 'main/eidolon/eidolon_voice_controller.cc').read_text()
refresh = definition(source, 'esp_err_t EidolonVoiceController::RefreshHubConfig(')
gate_source = (root / 'main/eidolon/output_policy.cc').read_text()
begin = gate_source.index('constexpr uint32_t kResponseOutputs=')
gate = gate_source[begin:gate_source.index(';', begin) + 1]
for method in ['bool DeviceOutputGate::Bind(', 'bool DeviceOutputGate::Start(',
               'void DeviceOutputGate::Close(', 'bool DeviceOutputGate::AllowsMicrophone(']:
    gate += '\n' + definition(gate_source, method)

code = r'''
#include <cassert>
#include <string>
#include <utility>
#include "eidolon/channel_recovery.h"
#define ESP_LOGW(...) ((void)0)
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_ERR_INVALID_STATE=-1, ESP_ERR_NOT_ALLOWED=-2;
namespace eidolon {
GATE
static DeviceOutputGate gate(false);
DeviceOutputGate& CurrentOutputGate() { return gate; }
static Esp32HubConfig response;
static int http_error=0, store_error=0, requests=0, writes=0;
struct HubOnboardingClient {
    int Resume(const std::string&, Esp32HubConfig& fresh) {
        ++requests; fresh=response; return http_error;
    }
};
struct HubConfigStore { int SaveHubConfig(const Esp32HubConfig&) { ++writes;return store_error; } };
enum class VoiceSessionState { ConfigReady, Unauthorized, Opening };
static void esp_timer_stop(void*) {}
class EidolonVoiceController {
public:
    std::string device_control_uri_="https://owner.test";
    Esp32HubConfig config_;
    bool config_refresh_required_=true;
    VoiceSessionState state_=VoiceSessionState::ConfigReady;
    int state_changes=0, closes=0, polls=0, network_lost=0;
    void* onboarding_poll_timer_=nullptr;
    struct { int disconnects=0; bool IsConnected() { return true; }
             void Disconnect() { ++disconnects; } } session_;
    std::string OperationalDeviceInstanceId() { return "device"; }
    void DoNetworkLost() { ++network_lost; }
    void SetState(VoiceSessionState state,const char*) { state_=state; ++state_changes; }
    VoiceSessionState StateForConfig(const Esp32HubConfig&) { return VoiceSessionState::ConfigReady; }
    void ScheduleOnboardingPoll() { ++polls; }
    void CloseConversationAudio() { ++closes; CurrentOutputGate().Close(); }
    int RefreshHubConfig(bool persist=true, bool* channel_changed=nullptr);
};
REFRESH
}
int main() {
    using namespace eidolon;
    EidolonVoiceController controller;
    auto& current=controller.config_;
    current.status=HubConfigStatus::Active;
    current.session.server_url="wss://room.test"; current.session.identity="device";
    current.session.room_name="room"; current.session.token="credential";
    const auto speech=OutputBit(presentation::Output::Speech);
    current.output_policy={true,7,speech,true,true};
    assert(CurrentOutputGate().Bind(current.output_policy));
    SessionOutputPlan plan;
    plan.session_id="confirmed"; plan.policy_revision=7; plan.selected=speech;
    assert(CurrentOutputGate().Start(plan,"confirmed"));
    assert(CurrentOutputGate().AllowsMicrophone());
    response=current; response.clock.Observe(1700000000000LL,50,55);
    bool changed=true;
    assert(controller.RefreshHubConfig(false,&changed)==ESP_OK && !changed);
    assert(controller.config_.clock.Now(55)==response.clock.Now(55));
    assert(controller.config_.clock.Now(55)>0);
    assert(CurrentOutputGate().AllowsMicrophone());
    assert(controller.state_changes==0 && controller.closes==0 && controller.session_.disconnects==0);
    assert(writes==0 && requests==1 && !controller.config_refresh_required_);
    // Explicit refresh persists even when a prior hint made RAM current.
    assert(controller.RefreshHubConfig(true,&changed)==ESP_OK && !changed);
    assert(writes==1 && CurrentOutputGate().AllowsMicrophone());
    assert(controller.state_changes==0 && controller.session_.disconnects==0);
    store_error=ESP_ERR_INVALID_STATE;
    assert(controller.RefreshHubConfig(true,&changed)==ESP_ERR_INVALID_STATE);
    assert(writes==2 && CurrentOutputGate().AllowsMicrophone());
    store_error=ESP_OK;
    response.output_policy.revision=8; response.output_policy.microphone=false;
    assert(controller.RefreshHubConfig(false,&changed)==ESP_OK && changed);
    assert(!CurrentOutputGate().AllowsMicrophone());
    assert(controller.session_.disconnects==0); // caller owns receipt + retirement
    assert(controller.config_.output_policy.revision==8 && writes==2);
    response.output_policy.revision=7;
    assert(controller.RefreshHubConfig(false,&changed)==ESP_ERR_INVALID_STATE);
    assert(controller.config_.output_policy.revision==8 && !CurrentOutputGate().AllowsMicrophone());
    response=controller.config_; response.output_policy.revision=9;
    assert(controller.RefreshHubConfig(false)==ESP_OK);
    assert(controller.session_.disconnects==1 && controller.closes==1); // ordinary caller unchanged
    // Recovering for a user-requested open must not flash the idle UI.
    controller.state_=VoiceSessionState::Opening;
    const int previous_changes=controller.state_changes;
    assert(controller.RefreshHubConfig(false)==ESP_OK);
    assert(controller.state_==VoiceSessionState::Opening);
    assert(controller.state_changes==previous_changes);
    http_error=ESP_ERR_NOT_ALLOWED; response.status=HubConfigStatus::RecoveryRequired;
    response.recovery_hint="reapprove";
    assert(controller.RefreshHubConfig(false,&changed)==ESP_ERR_NOT_ALLOWED);
    assert(controller.config_.status==HubConfigStatus::RecoveryRequired && controller.network_lost==1);
    assert(writes==2); // transport hints and recovery never persist
}
'''.replace('GATE', gate).replace('REFRESH', refresh)
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'snapshot.cc').write_text(code)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined', '-I', str(root / 'main'),
                    str(path / 'snapshot.cc'), '-o', str(path / 'snapshot')], check=True)
    subprocess.run([str(path / 'snapshot')], check=True)
print('PASS: unchanged config retains real audio gate; changed/stale policy closes it; recovery status preserved')
