#!/usr/bin/env python3
"""Run the production session request builder with its IO isolated.

The Host can time a conversation start only from the moment it hears the
request. session_open therefore carries how long the device took from the
person's press to sending it (action_to_send_ms), once per press.
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
                    for signature, name in [('esp_err_t', 'JoinRoom'),
                                            ('esp_err_t', 'PublishSessionRequest')])
code = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <cJSON.h>
#define ESP_LOGI(...) ((void)0)
namespace eidolon {
constexpr auto kSessionOpenType="session_open", kSessionCloseType="session_close";
constexpr auto kSessionConversationIdField="conversation_id";
constexpr auto kSessionControlRequestIdField="control_request_id";
constexpr auto kSessionActionToSendMsField="action_to_send_ms";
constexpr auto kSessionControlTopic="eidolon.session_control";
constexpr int kWireSchemaVersion=1, ESP_OK=0, ESP_ERR_NO_MEM=0x101;
using esp_err_t=int;
int64_t now_us=0;
int64_t esp_timer_get_time() { return now_us; }
const char* esp_err_to_name(int) { return "ESP_OK"; }
enum class EventType { Join };
struct Event { EventType type; };
struct Pending { std::string id; const std::string& control_request_id() const { return id; } };
class EidolonVoiceController {
public:
 std::atomic<int64_t> join_requested_us_{0};
 uint32_t session_generation_=1;
 Pending pending_room_join_;
 int state_=0;
 struct Session {
   std::string last;
   int PublishData(const char*,const char* payload,bool) { last=payload;return ESP_OK; }
 } session_;
 static const char* VoiceStateName(int) { return "test"; }
 const char* CurrentRoomKind() { return "control"; }
 esp_err_t Enqueue(const Event&) { return ESP_OK; }
 esp_err_t JoinRoom();
 esp_err_t PublishSessionRequest(const char*,const std::string&);
};
METHODS
}
using namespace eidolon;
static long long elapsed(const std::string& payload) {
  cJSON* root=cJSON_Parse(payload.c_str());
  const cJSON* item=cJSON_GetObjectItem(root,"action_to_send_ms");
  long long value=cJSON_IsNumber(item) ? static_cast<long long>(item->valuedouble) : -1;
  cJSON_Delete(root);
  return value;
}
int main() {
  EidolonVoiceController c;
  // A press, then the request 840 ms later (a reconnect in between, say).
  now_us=5000000;c.JoinRoom();
  now_us=5840000;c.PublishSessionRequest(kSessionOpenType,"a");
  assert(elapsed(c.session_.last)==840);
  // A retry of the same request is not a second press.
  now_us=6000000;c.PublishSessionRequest(kSessionOpenType,"a");
  assert(elapsed(c.session_.last)==-1);
  // A close never reports one, and does not consume the next press.
  now_us=7000000;c.JoinRoom();
  now_us=7010000;c.PublishSessionRequest(kSessionCloseType,"a");
  assert(elapsed(c.session_.last)==-1);
  now_us=7025000;c.PublishSessionRequest(kSessionOpenType,"b");
  assert(elapsed(c.session_.last)==25);
  // An open nobody pressed for (a Host room.join) says nothing about a press.
  now_us=9000000;c.PublishSessionRequest(kSessionOpenType,"c");
  assert(elapsed(c.session_.last)==-1);
  puts("PASS: session_open reports press-to-send once per press; retries, closes and Host joins do not");
}
'''.replace('\nMETHODS\n', '\n'+methods+'\n')
flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','libcjson'],text=True))
with tempfile.TemporaryDirectory() as work:
    path=Path(work); (path/'timing.cc').write_text(code)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-Wall','-Wextra','-Werror',
        '-Wno-unused-variable','-Wno-unused-function','-fsanitize=address,undefined',
        str(path/'timing.cc'),*flags,'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
