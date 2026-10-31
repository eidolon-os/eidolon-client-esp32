#!/usr/bin/env python3
"""Run production lifecycle handlers with the real output gate; IO is isolated.

No device, RTC server, or audio is used. These tests verify controller/gate
integration, not SDP interoperability or physical speaker output.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'main/eidolon/eidolon_voice_controller.cc').read_text()


def definition(needle):
    begin = source.index(needle)
    end = source.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end]


methods = '\n'.join(definition(signature + ' EidolonVoiceController::' + name + '(')
                    for signature, name in [
                        ('void', 'DoLiveKitState'), ('void', 'DoSessionControl'),
                        ('void', 'DoConnectTimeout'), ('void', 'DoNetworkLost'), ('bool', 'OpenConversationAudio'),
                        ('void', 'SuspendConversationAudio'), ('void', 'CloseConversationAudio'),
                        ('void', 'HandleSessionEnd')])
code = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <cJSON.h>
#include "eidolon/output_policy.h"
#include "eidolon/device_capabilities.h"
#include "eidolon/channel_recovery.h"
#include "eidolon/voice_session_state.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
namespace eidolon {
constexpr auto kSessionOpenType="session_open", kSessionCloseType="session_close";
constexpr auto kSessionStartedType="session_started", kSessionRejectedType="session_rejected";
constexpr auto kSessionRejectionConflict="conflict", kSessionConversationIdField="conversation_id";
constexpr auto kSessionRejectionUnavailable="unavailable";
constexpr auto kSessionEndIdleNormal="idle", kSessionEndProactiveDone="done", kSessionEndUserLeft="left";
constexpr auto kSessionEndSuperseded="superseded", kSessionEndError="error";
constexpr int kWireSchemaVersion=1, ESP_OK=0;
constexpr int LIVEKIT_FAILURE_REASON_ROOM_DELETED=1, LIVEKIT_FAILURE_REASON_ROOM_CLOSED=2;
using esp_err_t=int;
enum class EndReason { None, IdleNormalEnd, ProactiveDone, UserLeft, Superseded, Error, Busy };
enum class LiveKitConnectionState { Connecting, Connected, Reconnecting, Failed, Disconnected };
enum class HeadMotionStatus { Cancelled };
EndReason ParseEndReason(const std::string&) { return EndReason::None; }
void esp_timer_stop(void*) {}
int64_t esp_timer_get_time() { return 1000000; }
const char* livekit_failure_reason_str(int) { return "test"; }
const char* LiveKitConnectionStateName(LiveKitConnectionState) { return "test"; }
bool capture=false;
void eidolon_livekit_board_set_capture_enabled(bool enabled) { capture=enabled; }
struct Board { static Board& GetInstance() { static Board b;return b; } void HeadStop() {} };
struct View { void SetMotionDiagnostic(const char*,const char*,const char*) {} };
View* GetEidolonView() { return nullptr; }
struct Pending { bool active() const { return false; } bool Matches(uint32_t) const { return false; } };
struct Visit { struct Lease { bool OnConnected(int64_t) { return true; }
    int64_t deadline_us() { return 0; } } lease; int command=0; };
class EidolonVoiceController {
public:
 Visit* shared_visit_=nullptr;
 bool standby_=true,conversation_confirmed_=false,switching_to_voice_=false;
 uint32_t session_generation_=1;
 std::string current_conversation_id_="same-session",conversation_control_request_id_;
 VoiceSessionState state_=VoiceSessionState::Opening;
 EndReason last_end_reason_=EndReason::None;
 ChannelRecovery channel_recovery_;
 Esp32HubConfig config_;
 Pending pending_room_join_;
 void* reconnect_timer_=nullptr;
 void* ambient_presence_timer_=nullptr;
 void* onboarding_poll_timer_=nullptr;
 int opens=0,requests=0,request_error=0,retries=0;
 bool publisher=false;
 int64_t head_deadline_us_=0;
 int head_token_=0, head_command_=0;
 std::string head_gesture_;
 struct { HeadMotionStatus status; const char* reason; } head_result_;
 struct { void Reset() {} } session_playback_;
 struct Session {
   bool connected=true;
   bool IsConnected() { return connected; }
   void Disconnect() { connected=false; }
   int LastFailureReason() { return 0; }
 } session_;
 enum class AgentPhase { Silent }; AgentPhase agent_phase_=AgentPhase::Silent;
 void DoLiveKitState(LiveKitConnectionState,uint32_t);
 void DoSessionControl(const std::string&,uint32_t,bool);
 void DoConnectTimeout(); void DoNetworkLost();
 void SuspendConversationAudio(); void CloseConversationAudio(); bool OpenConversationAudio();
 void HandleSessionEnd(EndReason);
 void SetOperationalReady(bool,const char*) {}
 void DisarmConnectWatchdog() {}
 void ArmConnectWatchdog() {}
 void SetState(VoiceSessionState s,const char*) { state_=s; }
 VoiceSessionState StateForConfig(const Esp32HubConfig&) { return VoiceSessionState::ConfigReady; }
 std::string NewConversationId() { return "new-session-"+std::to_string(++ids); }
 int ids=0;
 int PublishSessionRequest(const char*,const std::string&) { ++requests;return request_error; }
 void StartAudioStatePublisher() { publisher=true;++opens; }
 void StopAudioStatePublisher() { publisher=false; }
 void PublishClientAudioState(bool) { capture=CurrentOutputGate().AllowsMicrophone(); }
 bool AgentOutputActiveRecently() { return false; }
 const char* VoiceStateName(VoiceSessionState) { return "test"; }
 void CompletePendingRoomJoinCommand(const char*,const char*,const char* = "",const char* = "") {}
 enum class PresenceWakePhase { Idle };
 void SetPresenceWakePhase(PresenceWakePhase) {}
 void FinishSharedVisit(const char*,bool=true) {}
 bool ArmSharedDeadline(int64_t) { return true; }
 void AckCommand(int,const char*,const char*,const char*,const char*) {}
 void ScheduleChannelReconnect(const char*) { ++retries; }
 void DoSharedDeadline() {}
 const char* CurrentRoomKind() { return "test"; }
 void MarkSessionSuperseded(const char*) { ++session_generation_; }
};
METHODS
}
std::string ack(const std::string& id, unsigned revision=1, bool microphone=true) {
 return "{\"schema_v\":1,\"type\":\"session_started\",\"conversation_id\":\""+id+
 "\",\"output_plan\":{\"session_id\":\""+id+"\",\"policy_revision\":"+std::to_string(revision)+
 ",\"outputs\":{\"speech\":true},\"inputs\":{\"microphone\":"+(microphone ? "true" : "false")+"}}}";
}
int main() {
 using namespace eidolon;
 auto& gate=CurrentOutputGate();
 const auto speech=OutputBit(presentation::Output::Speech);
 assert(gate.Bind({true,1,speech,true,true}));
 EidolonVoiceController c;
 c.config_.output_policy={true,1,speech,true,true};
 c.DoSessionControl(ack(c.current_conversation_id_),1,true);
 assert(c.state_==VoiceSessionState::InRoom && c.conversation_confirmed_ && capture);
 // SDK repair and full reconnect must both recover without a second start ack.
 for (int i=0;i<100;++i) {
   auto loss=(i%2) ? LiveKitConnectionState::Disconnected : LiveKitConnectionState::Reconnecting;
   c.session_.connected=false;c.DoLiveKitState(loss,1);
   assert(!gate.AllowsMicrophone() && !gate.Allows(presentation::Output::Speech) && !capture);
   c.DoSessionControl(ack(c.current_conversation_id_),1,true); // queued pre-loss ack
   assert(!capture && c.state_==VoiceSessionState::Reconnecting);
   c.session_.connected=true;c.DoLiveKitState(LiveKitConnectionState::Connected,1);
   assert(c.current_conversation_id_=="same-session");
   assert(c.state_==VoiceSessionState::InRoom && capture && gate.Allows(presentation::Output::Speech));
   c.DoSessionControl(ack(c.current_conversation_id_),1,true); // idempotent duplicate
   assert(capture);
 }
 // Watchdog abandons transport, not the grant; superseded callbacks do nothing.
 c.session_.connected=false;c.DoLiveKitState(LiveKitConnectionState::Reconnecting,1);
 c.DoConnectTimeout();
 assert(c.session_generation_==2 && gate.CanResume("same-session") && !capture);
 c.DoLiveKitState(LiveKitConnectionState::Connected,1);
 c.DoSessionControl(ack("same-session"),1,true);
 assert(!capture && c.state_==VoiceSessionState::Reconnecting);
 c.session_.connected=true;c.DoLiveKitState(LiveKitConnectionState::Connected,2);
 assert(capture && c.state_==VoiceSessionState::InRoom);
 // Changed permission cannot reuse the confirmed flag or old Provider dispatch.
 c.session_.connected=false;c.DoLiveKitState(LiveKitConnectionState::Failed,2);
 c.config_.output_policy={true,2,speech,true,false};
 assert(gate.Bind(c.config_.output_policy));
 c.session_.connected=true;c.DoLiveKitState(LiveKitConnectionState::Connected,2);
 assert(c.state_==VoiceSessionState::Opening && !c.conversation_confirmed_ && !capture);
 assert(c.current_conversation_id_!="same-session");
 c.DoSessionControl(ack("same-session"),2,true);
 assert(!capture && c.state_==VoiceSessionState::Opening);
 c.DoSessionControl(ack(c.current_conversation_id_,2,false),2,true);
 assert(c.state_==VoiceSessionState::InRoom && !capture && gate.Allows(presentation::Output::Speech));
 // Input-off sessions remain input-off across reconnect.
 c.session_.connected=false;c.DoLiveKitState(LiveKitConnectionState::Disconnected,2);
 c.session_.connected=true;c.DoLiveKitState(LiveKitConnectionState::Connected,2);
 assert(c.state_==VoiceSessionState::InRoom && !capture && gate.Allows(presentation::Output::Speech));
 // Leave during reconnect clears authority; late acknowledgements cannot revive it.
 const std::string ended=c.current_conversation_id_;
 c.session_.connected=false;c.DoLiveKitState(LiveKitConnectionState::Disconnected,2);
 c.HandleSessionEnd(EndReason::UserLeft);
 c.session_.connected=true;c.DoLiveKitState(LiveKitConnectionState::Connected,2);
 c.DoSessionControl(ack(ended,2,false),2,true);
 assert(c.current_conversation_id_.empty() && !c.conversation_confirmed_ && !capture);
 assert(!gate.CanResume(ended) && !gate.Allows(presentation::Output::Speech));
 // An unconfirmed open expires without leaving a reusable grant.
 c.current_conversation_id_="pending";c.state_=VoiceSessionState::Opening;
 c.DoConnectTimeout();
 assert(c.current_conversation_id_.empty() && !capture && !gate.CanResume("pending"));
 // Non-Agent confirmations cannot grant media, including during recovery.
 c.current_conversation_id_="forged";c.state_=VoiceSessionState::Opening;
 c.DoSessionControl(ack("forged",2,false),2,false);
 assert(c.current_conversation_id_.empty() && !gate.Allows(presentation::Output::Speech));
 // A failed re-announcement clears the suspended grant, not just the ID.
 c.current_conversation_id_="failure";c.state_=VoiceSessionState::Opening;
 c.DoSessionControl(ack("failure",2,false),2,true);
 c.session_.connected=false;c.DoLiveKitState(LiveKitConnectionState::Disconnected,2);
 c.request_error=-1;c.session_.connected=true;c.DoLiveKitState(LiveKitConnectionState::Connected,2);
 assert(c.state_==VoiceSessionState::Error && c.current_conversation_id_.empty());
 assert(!gate.CanResume("failure"));
 // A refused start ends the pending attempt now; its reason picks what the screen says.
 for (auto [reason, expected] : {std::pair{"conflict", EndReason::Busy},
                                 std::pair{"unavailable", EndReason::Error}}) {
   c.current_conversation_id_="refused";c.state_=VoiceSessionState::Opening;
   c.last_end_reason_=EndReason::None;
   const std::string refusal=std::string("{\"schema_v\":1,\"type\":\"session_rejected\","
       "\"conversation_id\":\"refused\",\"reason\":\"")+reason+"\"}";
   c.DoSessionControl(refusal,2,true);  // only the Provider refuses, never the Agent
   assert(c.current_conversation_id_=="refused" && c.state_==VoiceSessionState::Opening);
   c.DoSessionControl(refusal,2,false);
   assert(c.last_end_reason_==expected && c.current_conversation_id_.empty() && !capture);
 }
 // A reason this build does not know leaves the attempt to its own timeout.
 c.current_conversation_id_="unknown";c.state_=VoiceSessionState::Opening;
 c.DoSessionControl("{\"schema_v\":1,\"type\":\"session_rejected\",\"conversation_id\":\"unknown\","
     "\"reason\":\"later\"}",2,false);
 assert(c.current_conversation_id_=="unknown" && c.state_==VoiceSessionState::Opening);
 // Wi-Fi loss suspends the grant too; actual transport startup is isolated.
 EidolonVoiceController network;
 network.config_.output_policy={true,3,speech,true,true};
 assert(gate.Bind(network.config_.output_policy));
 network.DoSessionControl(ack("same-session",3,true),1,true);
 network.DoNetworkLost();
 assert(!capture && gate.CanResume("same-session") && network.session_generation_==2);
 network.standby_=true;network.session_.connected=true;
 network.DoLiveKitState(LiveKitConnectionState::Connected,2);
 assert(capture && network.current_conversation_id_=="same-session");
 puts("PASS: 100 reconnect cycles, watchdog, stale/offline/duplicate/untrusted controls, policy change, input-off, leave, open timeout, send failure and refused starts");
}
'''.replace('\nMETHODS\n', '\n'+methods+'\n')
flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','libcjson'],text=True))
with tempfile.TemporaryDirectory() as work:
    path=Path(work); (path/'recovery.cc').write_text(code)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-Wall','-Wextra','-Werror',
        '-Wno-unused-variable','-Wno-unused-function',
        '-fsanitize=address,undefined','-DCONFIG_EIDOLON_OUTPUT_POLICY_V1=1','-I',str(ROOT/'main'),
        str(path/'recovery.cc'),str(ROOT/'main/eidolon/output_policy.cc'),*flags,'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
