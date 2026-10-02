#!/usr/bin/env python3
"""Execute the production permission handler with isolated transport boundaries."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PermissionRefreshTest(unittest.TestCase):
    def test_owner_refresh_controls_reconnect_and_ui(self):
        source = (ROOT / 'main/eidolon/eidolon_voice_controller.cc').read_text()
        start = source.index('esp_err_t EidolonVoiceController::ConnectChannel()')
        boundary = source.index('    // One channel,', start)
        # Execute the actual authorization stage, isolating the transport stage.
        authorization = source[start:boundary] + '    return ESP_OK;\n}\n'
        harness = r'''
#include <cassert>
#include <string>
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_ERR_INVALID_STATE=-1;
enum class VoiceSessionState { Reconnecting };
class EidolonVoiceController {
public:
  bool shared_visit_=false, config_refresh_required_=true, active=true;
  bool owner_active=true, network=true, waiting_binding=false;
  int refresh_error=0, refreshes=0, reconnecting=0;
  std::string current_conversation_id_="confirmed-id";
  struct Recovery { bool* network;
    bool network_available() const { return *network; }
  } channel_recovery_{&network};
  bool HasChannelConfig() const { return active; }
  int RefreshHubConfig(bool) {
    ++refreshes;
    if (refresh_error) return refresh_error;
    config_refresh_required_=false;
    active=owner_active; waiting_binding=!active;
    return ESP_OK;
  }
  void SetState(VoiceSessionState,const char*) {
    ++reconnecting; waiting_binding=false;
  }
  esp_err_t ConnectChannel();
};
AUTHORIZATION
int main() {
  EidolonVoiceController active;
  assert(active.ConnectChannel()==ESP_OK && active.refreshes==1);
  assert(active.reconnecting==1 && !active.config_refresh_required_);
  EidolonVoiceController revoked;
  revoked.owner_active=false;
  assert(revoked.ConnectChannel()==ESP_ERR_INVALID_STATE);
  assert(revoked.waiting_binding && revoked.reconnecting==0);
  EidolonVoiceController unavailable;
  unavailable.refresh_error=-2;
  assert(unavailable.ConnectChannel()==-2);
  assert(unavailable.config_refresh_required_ && unavailable.reconnecting==0);
  unavailable.refresh_error=0;
  assert(unavailable.ConnectChannel()==ESP_OK && unavailable.refreshes==2);
  EidolonVoiceController offline;
  offline.network=false;
  assert(offline.ConnectChannel()==ESP_ERR_INVALID_STATE && offline.refreshes==0);
  EidolonVoiceController visit;
  visit.shared_visit_=true;
  assert(visit.ConnectChannel()==ESP_ERR_INVALID_STATE && visit.refreshes==0);
}
'''
        with tempfile.TemporaryDirectory() as work:
            path = Path(work) / 'permission_authorization.cc'
            binary = Path(work) / 'permission_authorization'
            path.write_text(harness.replace('AUTHORIZATION', authorization))
            subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17',
                            '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            str(path), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_preserve_desired_conversation_and_fail_closed(self):
        source = (ROOT / 'main/eidolon/eidolon_voice_controller.cc').read_text()
        start = source.index('void EidolonVoiceController::DoPermissionsChanged(')
        handler = source[start:source.index('\n}\n', start) + 3]
        harness = r'''
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#define ESP_LOGI(...) ((void)0)
constexpr int ESP_OK=0;
[[maybe_unused]] constexpr auto kSessionCloseType="close";
enum class VoiceSessionState { Reconnecting };
enum class EndReason { Superseded };
int64_t esp_timer_get_time() { return 100000000; }
class EidolonVoiceController {
public:
  uint32_t session_generation_=7;
  bool shared_visit_=false, standby_=false, config_refresh_required_=false;
  bool conversation_confirmed_=true, audio_open=true;
  std::string current_conversation_id_="confirmed-id", pending_join="request-id";
  std::vector<std::string> calls;
  int connect_error=ESP_OK, terminal_ends=0, closes_sent=0, shared_ends=0;
  struct Session { std::vector<std::string>* calls;
    void Disconnect() { calls->push_back("disconnect"); }
  } session_{&calls};
  struct Recovery { std::vector<std::string>* calls;
    void OnDisconnected(int64_t) { calls->push_back("recovery"); }
  } channel_recovery_{&calls};
  void CloseConversationAudio() { audio_open=false; calls.push_back("audio-close"); }
  void FinishSharedVisit(const char*) { ++shared_ends; }
  void SetState(VoiceSessionState, const char*) { calls.push_back("reconnecting"); }
  void DisarmConnectWatchdog() { calls.push_back("disarm"); }
  void MarkSessionSuperseded(const char*) { ++session_generation_; }
  void SetOperationalReady(bool ready,const char*) { assert(!ready); }
  int ConnectChannel() {
    assert(!audio_open && config_refresh_required_);
    assert(!calls.empty() && calls.front()=="audio-close");
    calls.push_back("fresh-owner-connect"); return connect_error;
  }
  void ScheduleChannelReconnect(const char*) { calls.push_back("backoff"); }
  void PublishSessionRequest(const char*, const std::string&) { ++closes_sent; }
  void HandleSessionEnd(EndReason) { ++terminal_ends; current_conversation_id_.clear(); }
  void CompletePendingRoomJoinCommand(const char*,const char*) { pending_join.clear(); }
  void DoPermissionsChanged(uint32_t generation);
};
HANDLER
int main() {
  EidolonVoiceController stale;
  stale.DoPermissionsChanged(6);
  assert(stale.calls.empty() && stale.audio_open);
  EidolonVoiceController active;
  active.DoPermissionsChanged(7);
  assert(active.current_conversation_id_=="confirmed-id");
  assert(active.conversation_confirmed_ && active.pending_join=="request-id");
  assert(active.terminal_ends==0 && active.closes_sent==0);
  assert(active.calls.back()=="fresh-owner-connect" && active.standby_);
  EidolonVoiceController failed;
  failed.connect_error=-1; failed.DoPermissionsChanged(7);
  assert(!failed.audio_open && failed.config_refresh_required_);
  assert(failed.current_conversation_id_=="confirmed-id");
  assert(failed.calls.back()=="backoff");
  EidolonVoiceController idle;
  idle.current_conversation_id_.clear(); idle.DoPermissionsChanged(7);
  assert(idle.current_conversation_id_.empty() && idle.terminal_ends==0);
  EidolonVoiceController visit;
  visit.shared_visit_=true; visit.DoPermissionsChanged(7);
  assert(!visit.audio_open && visit.shared_ends==1);
  assert(!visit.config_refresh_required_); // no standing-policy widening
}
'''
        with tempfile.TemporaryDirectory() as work:
            path = Path(work) / 'permission_refresh.cc'
            binary = Path(work) / 'permission_refresh'
            path.write_text(harness.replace('HANDLER', handler))
            subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17',
                            '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            str(path), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
            # Confirm the boundary test detects the previously unsafe terminal
            # end, rather than merely accepting any handler that reconnects.
            mutation = handler.replace('DisarmConnectWatchdog();',
                                       'HandleSessionEnd(EndReason::Superseded);\n'
                                       '    DisarmConnectWatchdog();')
            path.write_text(harness.replace('HANDLER', mutation))
            subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17',
                            str(path), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True)
            self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
