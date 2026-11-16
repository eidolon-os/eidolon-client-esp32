#!/usr/bin/env python3
"""Compile the production callback against the SDK's ParticipantInfo layout."""
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
#include "eidolon/policy_announcement.h"
#define ESP_LOGI(...) ((void)0)
using namespace eidolon;
constexpr int LIVEKIT_PARTICIPANT_STATE_JOINING=0, LIVEKIT_PARTICIPANT_STATE_JOINED=1,
    LIVEKIT_PARTICIPANT_STATE_ACTIVE=2, LIVEKIT_PARTICIPANT_STATE_DISCONNECTED=3;
constexpr int LIVEKIT_PARTICIPANT_KIND_AGENT=4;
// Official v0.3.11 fields plus the attributes the vendored fork decodes;
// deliberately no locality or permissions extension.
struct livekit_participant_attribute_t { const char *key, *value; };
struct livekit_participant_info_t {
    const char *sid=nullptr, *identity=nullptr, *name=nullptr, *metadata=nullptr;
    int kind=0, state=0;
    const livekit_participant_attribute_t* attributes=nullptr;
    size_t attributes_count=0;
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
    std::function<void(uint32_t,PolicyAnnouncement)> on_configuration_invalidated_;
    std::function<void(const std::string&,uint32_t,bool)> on_session_control_;
    static void OnParticipantInfo(const livekit_participant_info_t*,void*);
};
CALLBACK
int main() {
    LiveKitSession session;
    int hints=0;
    PolicyAnnouncement last;
    session.on_configuration_invalidated_=[&](uint32_t generation, PolicyAnnouncement announcement) {
        assert(generation==7); ++hints; last=announcement;
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
    assert(hints==2 && !last.announced);
    // A current Host names the Owner policy revision among other attributes.
    const livekit_participant_attribute_t attributes[]={
        {"lk.agent.state","listening"},{"eidolon.policy_revision","9"}};
    info.attributes=attributes; info.attributes_count=2;
    LiveKitSession::OnParticipantInfo(&info,&session);
    assert(hints==3 && last.announced && last.revision==9);
    const livekit_participant_attribute_t unreadable[]={{"eidolon.policy_revision","9a"}};
    info.attributes=unreadable; info.attributes_count=1;
    LiveKitSession::OnParticipantInfo(&info,&session);
    assert(hints==4 && !last.announced);
    info.attributes=nullptr; info.attributes_count=0;
    hints=2;
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
                    '-Werror', '-fsanitize=address,undefined', '-I', str(root / 'main'),
                    str(path / 'callback.cc'),
                    '-o', str(path / 'callback')], check=True)
    subprocess.run([str(path / 'callback')], check=True)
print('PASS: SDK callback layout, local/remote/state boundaries, announced revision, unlocked notification')
