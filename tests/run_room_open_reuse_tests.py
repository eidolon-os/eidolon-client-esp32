#!/usr/bin/env python3
"""Exercise the actual room-open controller against fake Hub and transport IO."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'main/eidolon/eidolon_voice_controller.cc').read_text()
a = source.index('esp_err_t EidolonVoiceController::DoJoinRoom()')
b = source.index('\nesp_err_t EidolonVoiceController::ConnectChannel()', a)
body = source[a:b]
code = r'''
#include <cassert>
#include <string>
#include "eidolon/channel_recovery.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
using esp_err_t=int;
constexpr int ESP_OK=0, ESP_ERR_INVALID_STATE=-1, ESP_ERR_NOT_FOUND=-2, ESP_ERR_NOT_ALLOWED=-3;
int64_t esp_timer_get_time() { return 1000000; }
namespace eidolon {
enum class VoiceSessionState { ConfigReady, Connecting, Opening, Reconnecting, InRoom, Unauthorized, Error };
enum class EndReason { None };
constexpr auto kSessionOpenType="open";
class EidolonVoiceController {
public:
    bool shared_visit_=false, memory_ceiling_announced_=false, standby_=true;
    bool config_refresh_required_=false, conversation_confirmed_=false;
    int session_generation_=1, refreshes=0, connects=0, opens=0, retries=0;
    int refresh_error=0;
    Esp32HubConfig config_;
    VoiceSessionState state_=VoiceSessionState::ConfigReady;
    EndReason last_end_reason_=EndReason::None;
    std::string pending_session_intent_, current_conversation_id_, conversation_control_request_id_;
    struct Session { bool connected=true; bool IsConnected() {return connected;}
        void ForgetInternalMemoryCeiling() {} } session_;
    EidolonVoiceController() {
        config_.status=HubConfigStatus::Active;
        config_.session.server_url="wss://test"; config_.session.room_name="room";
        config_.session.token="token"; config_.session.identity="device";
        config_.clock.Observe(1700000000000LL,0,0);
        config_.expires_at_ms=1700000060000LL;
    }
    int LoadStoredConfig() { return ESP_ERR_NOT_FOUND; }
    int RefreshHubConfig(bool persist) {
        assert(!persist && state_==VoiceSessionState::Opening);
        ++refreshes;
        if (!refresh_error) {
            config_refresh_required_=false;
            config_.expires_at_ms=1700000060000LL;
        } else if (refresh_error==ESP_ERR_NOT_ALLOWED) {
            state_=VoiceSessionState::Unauthorized;
        }
        return refresh_error;
    }
    bool HasActiveConfig() { return config_.status==HubConfigStatus::Active &&
        config_.session.usable() && !ChannelBindingExpired(config_,1000); }
    void SetState(VoiceSessionState state, const char*) {state_=state;}
    VoiceSessionState StateForConfig(const Esp32HubConfig&) {return VoiceSessionState::ConfigReady;}
    std::string NewConversationId() {return "conversation";}
    int ConnectChannel() {++connects;return ESP_OK;}
    int PublishSessionRequest(const char*,const std::string&) {++opens;return ESP_OK;}
    void ScheduleChannelReconnect(const char*) {++retries;}
    int DoJoinRoom();
};
BODY
}
int main() {
    using namespace eidolon;
    { EidolonVoiceController c; assert(c.DoJoinRoom()==ESP_OK);
      assert(c.refreshes==0 && c.opens==1 && c.connects==0);
      assert(c.state_==VoiceSessionState::Opening && !c.conversation_confirmed_); }
    { EidolonVoiceController c; c.session_.connected=false;
      assert(c.DoJoinRoom()==ESP_OK && c.refreshes==1 && c.connects==1 && c.opens==0); }
    { EidolonVoiceController c; c.config_.expires_at_ms=1700000001000LL;
      assert(c.DoJoinRoom()==ESP_OK && c.refreshes==1 && c.opens==1); }
    { EidolonVoiceController c; c.config_refresh_required_=true;
      c.refresh_error=-5;
      assert(c.DoJoinRoom()==-5 && c.opens==0 && c.config_refresh_required_);
      assert(c.DoJoinRoom()==-5 && c.refreshes==2 && c.opens==0);
      c.refresh_error=ESP_OK;
      assert(c.DoJoinRoom()==ESP_OK && c.refreshes==3 && c.opens==1); }
    { EidolonVoiceController c; c.state_=VoiceSessionState::Unauthorized;
      c.refresh_error=ESP_ERR_NOT_ALLOWED;
      assert(c.DoJoinRoom()==ESP_ERR_NOT_ALLOWED && c.opens==0);
      assert(c.state_==VoiceSessionState::Unauthorized); }
    { EidolonVoiceController c; c.config_refresh_required_=true;
      c.config_.status=HubConfigStatus::WaitingBinding;
      assert(c.DoJoinRoom()==ESP_ERR_INVALID_STATE && c.opens==0); }
}
'''.replace('BODY', body)
with tempfile.TemporaryDirectory() as work:
    path = Path(work) / 'open.cc'
    binary = Path(work) / 'open'
    path.write_text(code)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra',
                    '-Werror', '-Wno-unused-variable', '-fsanitize=address,undefined',
                    '-I', str(root / 'main'), str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('room open: standing reuse, expired/disconnected recovery, failed refresh, revocation, rebind PASS')
