#!/usr/bin/env python3
"""Compile the production callback against the official ParticipantInfo layout."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'main/eidolon/livekit_session.cc').read_text()
begin = source.index('void LiveKitSession::OnParticipantInfo(')
callback = source[begin:source.index('\n}\n', begin) + 3]
code = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <array>
#include <mutex>
#include <functional>
#define ESP_LOGI(...) ((void)0)
constexpr int LIVEKIT_PARTICIPANT_STATE_JOINING=0, LIVEKIT_PARTICIPANT_STATE_JOINED=1,
    LIVEKIT_PARTICIPANT_STATE_ACTIVE=2, LIVEKIT_PARTICIPANT_STATE_DISCONNECTED=3;
constexpr int LIVEKIT_PARTICIPANT_KIND_AGENT=4;
// Official v0.3.11 fields; deliberately no locality or permissions extension.
struct livekit_participant_info_t {
    const char *sid=nullptr, *identity=nullptr, *name=nullptr, *metadata=nullptr;
    int kind=0, state=0;
};
int64_t esp_timer_get_time() { return 1000; }
class LiveKitSession {
public:
    std::mutex peers_mutex_;
    uint32_t generation_=7;
    std::string identity_="device";
    std::array<std::string,4> agent_peers_;
    struct Pending {
        std::string Resolve(const char*,bool,uint32_t,int64_t) { return {}; }
    } pending_session_control_;
    std::function<void(uint32_t)> on_configuration_invalidated_;
    std::function<void(const std::string&,uint32_t,bool)> on_session_control_;
    static void OnParticipantInfo(const livekit_participant_info_t*,void*);
};
CALLBACK
int main() {
    LiveKitSession session;
    int hints=0;
    session.on_configuration_invalidated_=[&](uint32_t generation) {
        assert(generation==7); ++hints;
        // The callback must leave its lock before notifying the controller.
        assert(session.peers_mutex_.try_lock()); session.peers_mutex_.unlock();
    };
    livekit_participant_info_t info;
    info.identity="device";
    for (int state: {LIVEKIT_PARTICIPANT_STATE_JOINING,
                     LIVEKIT_PARTICIPANT_STATE_JOINED, LIVEKIT_PARTICIPANT_STATE_DISCONNECTED}) {
        info.state=state; LiveKitSession::OnParticipantInfo(&info,&session);
    }
    assert(hints==0);
    info.state=2;
    LiveKitSession::OnParticipantInfo(&info,&session); // first ACTIVE must check the join race too
    LiveKitSession::OnParticipantInfo(&info,&session); // duplicate/permission-only updates are hints
    assert(hints==2);
    info.identity="remote";
    LiveKitSession::OnParticipantInfo(&info,&session); assert(hints==2);
    info.kind=4;
    LiveKitSession::OnParticipantInfo(&info,&session);
    assert(session.agent_peers_[0]=="remote");
    info.state=3;
    LiveKitSession::OnParticipantInfo(&info,&session);
    assert(session.agent_peers_[0].empty());
    info.identity=nullptr;
    LiveKitSession::OnParticipantInfo(&info,&session);
    LiveKitSession::OnParticipantInfo(nullptr,&session);
    info.identity="device"; info.state=2;
    LiveKitSession::OnParticipantInfo(&info,nullptr);
    session.identity_.clear();
    LiveKitSession::OnParticipantInfo(&info,&session);
    assert(hints==2);
}
'''.replace('CALLBACK', callback)
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'callback.cc').write_text(code)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined', str(path / 'callback.cc'),
                    '-o', str(path / 'callback')], check=True)
    subprocess.run([str(path / 'callback')], check=True)
print('PASS: official callback layout, local/remote/state boundaries, unlocked notification')
