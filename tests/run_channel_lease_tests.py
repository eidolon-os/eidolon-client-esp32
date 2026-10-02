#!/usr/bin/env python3
"""Run the production controller loop with a virtual clock and transport boundary."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'main/eidolon/eidolon_voice_controller.cc').read_text()
start = source.index('void EidolonVoiceController::ControllerLoop()')
end = source.index('\nvoid EidolonVoiceController::DispatchAndRelease', start)
loop = source[start:end]
harness = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <vector>
#include "eidolon/channel_recovery.h"
#define ESP_LOGI(...) ((void)0)
using TickType_t = uint32_t;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
constexpr int portTICK_PERIOD_MS = 10;
struct Stop {};
int64_t now_ms = 0;
int64_t esp_timer_get_time() { return now_ms * 1000; }
namespace eidolon {
enum class EventType { ConfigurationInvalidated, SessionActivity, AudioTick };
struct Event { EventType type{}; uint32_t generation=0; };
template<class E> struct ControllerEventInbox {
    static constexpr uint32_t kConfigurationInvalidated=1, kSessionActivity=2, kAudioTick=4;
    int calls=0, max_calls=3; bool flood=false;
    std::vector<TickType_t> waits;
    uint32_t Wait(TickType_t timeout=portMAX_DELAY) {
        waits.push_back(timeout);
        if (++calls > max_calls || timeout==portMAX_DELAY) throw Stop{};
        now_ms += flood ? 1 : int64_t(timeout)*portTICK_PERIOD_MS;
        return 0;
    }
    bool Take(E&) { return flood; }
};
class EidolonVoiceController {
public:
    struct Session { bool connected=true; bool IsConnected() { return connected; } } session_;
    ControllerEventInbox<Event> inbox_;
    Esp32HubConfig config_;
    bool shared_visit_=false, authorized=true, fail_refresh=false, unchanged=false;
    int64_t channel_lease_retry_at_ms_=0;
    std::vector<int64_t> refresh_times;
    uint32_t session_generation_=1;
    std::atomic<uint32_t> configuration_generation_{1};
    int refreshes=0, commands=0;
    EidolonVoiceController() {
        now_ms=0; config_.status=HubConfigStatus::Active;
        config_.clock.Observe(1700000000000LL,0,0);
        config_.expires_at_ms=config_.clock.Now(0)+50;
    }
    bool CanRecoverChannel() { return authorized; }
    void DispatchAndRelease(const Event&) { ++commands; }
    void DoConfigurationInvalidated(uint32_t generation) {
        assert(generation==session_generation_); ++refreshes;
        refresh_times.push_back(now_ms);
        assert(now_ms>=50);
        if (fail_refresh) session_.connected=false;
        else if (!unchanged) config_.expires_at_ms=config_.clock.Now(now_ms)+50;
    }
    void ControllerLoop();
    void Run() { try { ControllerLoop(); } catch (Stop&) {} }
};
LOOP
}
int main() {
    using namespace eidolon;
    { EidolonVoiceController c; c.Run(); assert(c.refreshes==3 && c.commands==0); }
    { EidolonVoiceController c; c.inbox_.flood=true; c.inbox_.max_calls=120;
      c.Run(); assert(c.refreshes==2 && c.commands==120); }
    { EidolonVoiceController c; c.unchanged=true; c.Run();
      assert((c.refresh_times==std::vector<int64_t>{50,1050,2050})); }
    { EidolonVoiceController c; c.unchanged=true; c.inbox_.flood=true;
      c.inbox_.max_calls=2200; c.Run();
      assert((c.refresh_times==std::vector<int64_t>{50,1050,2050})); }
    { EidolonVoiceController c; c.fail_refresh=true; c.Run();
      assert(c.refreshes==1 && c.inbox_.waits.back()==portMAX_DELAY); }
    { EidolonVoiceController c; c.authorized=false; c.Run(); assert(c.refreshes==0); }
    { EidolonVoiceController c; c.shared_visit_=true; c.Run(); assert(c.refreshes==0); }
    { EidolonVoiceController c; c.session_.connected=false; c.Run(); assert(c.refreshes==0); }
    { EidolonVoiceController c; c.config_.clock={}; c.Run(); assert(c.refreshes==0); }
    { EidolonVoiceController c; c.config_.expires_at_ms=c.config_.clock.Now(0)+51;
      c.inbox_.max_calls=1; c.Run(); assert(now_ms==60 && c.refreshes==1); }
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)/'lease.cc'; binary = Path(tmp)/'lease'
    path.write_text(harness.replace('LOOP',loop))
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-I',str(root/'main'),str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('channel lease actor: idle renewal, command flood, failure handoff, revocation, shared visit, unknown clock, tick rounding, conservative clock retry pacing PASS')
